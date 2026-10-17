#pragma once
#include "../dsp/percussion.h"

// Synthesized snare: a two-mode "drum head" plus band-limited "snares" noise. Overview and
// parameter table: docs/percussion-instruments.md.
//
//   sine x2 (f, 1.83 f) <- fast pitch drop  --- body env (short)      \
//   noise -> band-pass (centre rises with Snap) - noise env (~Decay)   +-> tanh -> Panner
//   noise burst (3 ms), Snap-scaled ------------------------------   /
//
// Body and noise have separate envelopes and the body is always shorter than the rattle, which
// is most of what separates a snare from "noise with a thump". Snap trades body for crack.
namespace webdsp {

inline constexpr InstrumentParamInfo kSnareParams[] = {
    {static_cast<int32_t>(SnareParam::Tune), "tune", 100.0f, 400.0f, 185.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(SnareParam::Decay), "decay", 0.05f, 0.8f, 0.22f, "s", ModDomain::Log},
    {static_cast<int32_t>(SnareParam::Snap), "snap", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(SnareParam::Noise), "noise", 0.0f, 1.0f, 0.6f, "", ModDomain::Linear},
};
inline constexpr int32_t kSnareParamCount =
    static_cast<int32_t>(sizeof(kSnareParams) / sizeof(kSnareParams[0]));

class SnareInstrument final : public Instrument {
 public:
  SnareInstrument() {
    for (int32_t i = 0; i < kSnareParamCount; i++) values_[i] = kSnareParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Snare; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    bodyEnv_.configure(sampleRate);
    noiseEnv_.configure(sampleRate);
    snapEnv_.configure(sampleRate);
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    if (core_.begin(velocity, delayFrames, pan, pitch)) {
      phase1_ = phase2_ = 0.25;  // sine peak: the attack ramp, not the phase, avoids a click
      band_.reset();
    }
    const double sr = core_.sampleRate;
    noise_.reset();
    pitchEnv_ = 1.0f;
    const float decay = value(SnareParam::Decay);
    bodyEnv_.trigger(percDecayCoef(decay * kDecayToTau * 0.55f, sr));
    noiseEnv_.trigger(percDecayCoef(decay * kDecayToTau, sr));
    snapEnv_.trigger(percDecayCoef(0.003f, sr), 0.0002f);
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    const float r = percDecayCoef(0.004f, core_.sampleRate);
    bodyEnv_.setDecay(r);
    noiseEnv_.setDecay(r);
    snapEnv_.setDecay(r);
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kSnareParams, kSnareParamCount, values_, param, v);
  }

  void reset() override {
    bodyEnv_.reset();
    noiseEnv_.reset();
    snapEnv_.reset();
    band_.reset();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* mono = scratch[0];
    const double sr = core_.sampleRate;
    const double twoPi = 6.283185307179586;

    const double tune = std::min<double>(value(SnareParam::Tune) * core_.pitchRatio, sr * 0.2);
    const float snap = value(SnareParam::Snap);
    const float noiseAmt = value(SnareParam::Noise);
    const float pitchCoef = percDecayCoef(0.012f, sr);
    // Noise band: ~2.2 kHz (dull, "brush") .. ~5 kHz (crack); follows note pitch.
    band_.set((2200.0 + 2800.0 * snap) * std::sqrt(core_.pitchRatio), 0.9, sr);
    const float bodyGain = (0.9f - 0.45f * noiseAmt) * core_.velocity;
    const float noiseGain = (0.25f + 0.9f * noiseAmt) * core_.velocity;
    const float snapGain = snap * 0.9f * core_.velocity;

    bool finished = false;
    for (int32_t i = 0; i < numFrames; i++) {
      if (core_.pendingDelay > 0) {
        core_.pendingDelay--;
        mono[i] = 0.0f;
        continue;
      }
      if (!finished && bodyEnv_.done() && noiseEnv_.done() && snapEnv_.done()) finished = true;
      if (finished) { mono[i] = 0.0f; continue; }

      const double f = std::min(tune * (1.0 + 0.4 * pitchEnv_), sr * 0.45);
      pitchEnv_ *= pitchCoef;
      phase1_ += f / sr;
      phase2_ += f * 1.83 / sr;
      phase1_ -= std::floor(phase1_);
      phase2_ -= std::floor(phase2_);
      const float body = static_cast<float>(std::sin(twoPi * phase1_) +
                                            0.5 * std::sin(twoPi * phase2_)) *
                         bodyEnv_.next() * bodyGain * 0.65f;

      const float n = band_.process(noise_.next());
      const float rattle = n * (noiseEnv_.next() * noiseGain + snapEnv_.next() * snapGain);

      mono[i] = std::tanh(1.4f * (body + rattle * 1.6f)) * kLevel;
    }
    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr float kLevel = 0.8f;
  float value(SnareParam p) const { return values_[static_cast<int32_t>(p)]; }

  float values_[kSnareParamCount] = {};
  PercCore core_;
  PercEnv bodyEnv_, noiseEnv_, snapEnv_;
  PercNoise noise_;
  PercBandpass band_;
  double phase1_ = 0.25, phase2_ = 0.25;
  float pitchEnv_ = 0.0f;
};

}  // namespace webdsp
