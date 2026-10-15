#pragma once
#include "../dsp/percussion.h"

// Synthesized tom: a tuned membrane. Where the Kick is one swept sine, a tom keeps a clearly
// pitched fundamental and adds the inharmonic membrane modes (~1.59x and ~2.14x the
// fundamental) that make it read as a drum head rather than a sub. Overview:
// docs/percussion-instruments.md.
//
//   modes: sine f, 1.59 f, 2.14 f  <- shared strike pitch drop (linear in Hz, like Kick)
//   each mode has its own decay (upper modes die faster)             <- Decay, Body
//   + stick tick (short noise)                                       <- Body
//   -> BiquadFilter low-pass (Damp: darker + shorter) -> tanh -> Panner
//
// Note pitch is a semitone offset on Tune, so a tracker can play melodic tom fills.
namespace webdsp {

inline constexpr InstrumentParamInfo kTomParams[] = {
    {static_cast<int32_t>(TomParam::Tune), "tune", 60.0f, 400.0f, 120.0f, "Hz"},
    {static_cast<int32_t>(TomParam::PitchDrop), "pitchDrop", 0.0f, 2.0f, 0.6f, "oct"},
    {static_cast<int32_t>(TomParam::Decay), "decay", 0.1f, 1.5f, 0.45f, "s"},
    {static_cast<int32_t>(TomParam::Body), "body", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(TomParam::Damp), "damp", 0.0f, 1.0f, 0.3f, ""},
};
inline constexpr int32_t kTomParamCount =
    static_cast<int32_t>(sizeof(kTomParams) / sizeof(kTomParams[0]));

class TomInstrument final : public Instrument {
 public:
  TomInstrument() {
    for (int32_t i = 0; i < kTomParamCount; i++) values_[i] = kTomParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Tom; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    for (PercEnv& e : env_) e.configure(sampleRate);
    tickEnv_.configure(sampleRate);
    lowpass_ = BiquadFilter(sampleRate);
    lowpass_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), 12000.0f);
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    if (core_.begin(velocity, delayFrames, pan, pitch)) {
      for (double& p : phase_) p = 0.25;
      lowpass_.reset();
    }
    const double sr = core_.sampleRate;
    noise_.reset();
    pitchEnv_ = 1.0f;
    const float damp = value(TomParam::Damp);
    // Damping shortens the ring (a muffled head), on top of darkening it below.
    const float tau = value(TomParam::Decay) * kDecayToTau * (1.0f - 0.55f * damp);
    env_[0].trigger(percDecayCoef(tau, sr));
    env_[1].trigger(percDecayCoef(tau * 0.45f, sr));
    env_[2].trigger(percDecayCoef(tau * 0.25f, sr));
    tickEnv_.trigger(percDecayCoef(0.0025f, sr), 0.0002f);
    // Cutoff 12 kHz (open) .. 600 Hz (heavily damped), exponential in Damp.
    lowpass_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff),
                      12000.0f * std::pow(0.05f, damp));
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    const float r = percDecayCoef(0.004f, core_.sampleRate);
    for (PercEnv& e : env_) e.setDecay(r);
    tickEnv_.setDecay(r);
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kTomParams, kTomParamCount, values_, param, v);
  }

  void reset() override {
    for (PercEnv& e : env_) e.reset();
    tickEnv_.reset();
    lowpass_.reset();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* mono = scratch[0];
    const double sr = core_.sampleRate;
    const double twoPi = 6.283185307179586;

    const double tune = value(TomParam::Tune) * core_.pitchRatio;
    const double extra = std::exp2(static_cast<double>(value(TomParam::PitchDrop))) - 1.0;
    // Strike sweep: short for a small/fast tom, a bit longer for a long ring.
    const float pitchCoef = percDecayCoef(0.015f + 0.05f * value(TomParam::Decay), sr);
    const float body = value(TomParam::Body);
    const float level[kModes] = {1.0f, 0.55f * body, 0.3f * body};
    const float tickGain = (0.05f + 0.3f * body) * core_.velocity;
    const float gain = core_.velocity * kLevel;
    static constexpr double kRatio[kModes] = {1.0, 1.59, 2.14};

    const int32_t live = std::min(core_.pendingDelay, numFrames);
    bool finished = false;
    for (int32_t i = 0; i < numFrames; i++) {
      if (core_.pendingDelay > 0) {
        core_.pendingDelay--;
        mono[i] = 0.0f;
        continue;
      }
      if (!finished && env_[0].done() && tickEnv_.done()) finished = true;
      if (finished) { mono[i] = 0.0f; continue; }

      const double f = tune * (1.0 + extra * pitchEnv_);
      pitchEnv_ *= pitchCoef;
      float s = 0.0f;
      for (int32_t m = 0; m < kModes; m++) {
        phase_[m] += std::min(f * kRatio[m], sr * 0.45) / sr;
        phase_[m] -= std::floor(phase_[m]);
        s += static_cast<float>(std::sin(twoPi * phase_[m])) * env_[m].next() * level[m];
      }
      mono[i] = (s / (1.0f + 0.35f * body)) * gain + noise_.next() * tickEnv_.next() * tickGain;
    }

    // Damp low-pass over the live portion only (keeps the pre-note delay exactly silent).
    float* ch[1] = {mono + live};
    lowpass_.process(ch, 1, numFrames - live);
    for (int32_t i = live; i < numFrames; i++) mono[i] = std::tanh(mono[i]);

    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr int32_t kModes = 3;
  static constexpr float kLevel = 0.85f;
  float value(TomParam p) const { return values_[static_cast<int32_t>(p)]; }

  float values_[kTomParamCount] = {};
  PercCore core_;
  PercEnv env_[kModes], tickEnv_;
  PercNoise noise_;
  BiquadFilter lowpass_{48000.0};
  double phase_[kModes] = {0.25, 0.25, 0.25};
  float pitchEnv_ = 0.0f;
};

}  // namespace webdsp
