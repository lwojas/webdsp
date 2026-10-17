#pragma once
#include "../dsp/percussion.h"

// Synthesized hi-hat: the classic metallic-cluster approach rather than high-passed noise.
// Overview: docs/percussion-instruments.md.
//
//   6 square oscillators at inharmonic ratios (808-style, PolyBLEP anti-aliased)  <- Metal
//   + noise (share falls as Metal rises)
//   -> band-pass (Tone) -> BiquadFilter high-pass (Tone) -> two-stage env -> tanh
//   two-stage env: a ~5 ms "chick" + the Decay body; long Decay = open hat, short = closed.
//
// Monophonic, so a short hat after an open one retriggers and chokes it like hardware.
namespace webdsp {

inline constexpr InstrumentParamInfo kHiHatParams[] = {
    {static_cast<int32_t>(HiHatParam::Tune), "tune", -12.0f, 12.0f, 0.0f, "st", ModDomain::Linear},
    {static_cast<int32_t>(HiHatParam::Decay), "decay", 0.02f, 1.2f, 0.08f, "s", ModDomain::Log},
    {static_cast<int32_t>(HiHatParam::Metal), "metal", 0.0f, 1.0f, 0.7f, "", ModDomain::Linear},
    {static_cast<int32_t>(HiHatParam::Tone), "tone", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
};
inline constexpr int32_t kHiHatParamCount =
    static_cast<int32_t>(sizeof(kHiHatParams) / sizeof(kHiHatParams[0]));

class HiHatInstrument final : public Instrument {
 public:
  HiHatInstrument() {
    for (int32_t i = 0; i < kHiHatParamCount; i++) values_[i] = kHiHatParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::HiHat; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    bodyEnv_.configure(sampleRate);
    chickEnv_.configure(sampleRate);
    highpass_ = BiquadFilter(sampleRate);
    highpass_.setParam(static_cast<int32_t>(VoiceParam::FilterMode),
                       static_cast<float>(FilterMode::HighPass));
    highpass_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), 6000.0f);
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    if (core_.begin(velocity, delayFrames, pan, pitch)) {
      for (double& p : phase_) p = 0.0;
      band_.reset();
      highpass_.reset();
    }
    const double sr = core_.sampleRate;
    noise_.reset();
    bodyEnv_.trigger(percDecayCoef(value(HiHatParam::Decay) * kDecayToTau, sr), 0.0003f);
    chickEnv_.trigger(percDecayCoef(0.005f, sr), 0.0002f);
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    const float r = percDecayCoef(0.004f, core_.sampleRate);
    bodyEnv_.setDecay(r);
    chickEnv_.setDecay(r);
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kHiHatParams, kHiHatParamCount, values_, param, v);
  }

  void reset() override {
    bodyEnv_.reset();
    chickEnv_.reset();
    band_.reset();
    highpass_.reset();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* mono = scratch[0];
    const double sr = core_.sampleRate;

    const double ratio = core_.pitchRatio * std::exp2(value(HiHatParam::Tune) / 12.0);
    const float metal = value(HiHatParam::Metal);
    const float tone = value(HiHatParam::Tone);
    double inc[kOsc];
    for (int32_t o = 0; o < kOsc; o++) inc[o] = std::min(kFreq[o] * ratio, sr * 0.4) / sr;

    // Tone: band centre 6..11 kHz, high-pass 3.5..8 kHz (both follow note pitch). Metal also
    // sharpens the band so the cluster rings rather than hisses.
    const double bandHz = (6000.0 + 5000.0 * tone) * std::sqrt(ratio);
    band_.set(bandHz, 0.7 + 1.6 * metal, sr);
    highpass_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff),
                       static_cast<float>(std::min((3500.0 + 4500.0 * tone) * std::sqrt(ratio),
                                                   sr * 0.45)));
    const float metalGain = metal * 0.6f;
    const float noiseGain = (1.0f - 0.7f * metal) * 1.5f;
    const float gain = core_.velocity * kLevel;

    const int32_t live = std::min(core_.pendingDelay, numFrames);
    bool finished = false;
    for (int32_t i = 0; i < numFrames; i++) {
      if (core_.pendingDelay > 0) {
        core_.pendingDelay--;
        mono[i] = 0.0f;
        continue;
      }
      if (!finished && bodyEnv_.done() && chickEnv_.done()) finished = true;
      if (finished) { mono[i] = 0.0f; continue; }

      float cluster = 0.0f;
      for (int32_t o = 0; o < kOsc; o++) {
        phase_[o] += inc[o];
        phase_[o] -= std::floor(phase_[o]);
        cluster += squareBlep(phase_[o], inc[o]);
      }
      const float raw = cluster * metalGain + noise_.next() * noiseGain;
      const float env = 0.7f * bodyEnv_.next() + 0.6f * chickEnv_.next();
      mono[i] = band_.process(raw) * env * gain;
    }

    float* ch[1] = {mono + live};
    highpass_.process(ch, 1, numFrames - live);
    for (int32_t i = live; i < numFrames; i++) mono[i] = std::tanh(1.5f * mono[i]) * 0.8f;

    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr int32_t kOsc = 6;
  static constexpr float kLevel = 0.9f;
  // The well-known 808 hat oscillator frequencies (Hz): mutually inharmonic.
  static constexpr double kFreq[kOsc] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};

  float value(HiHatParam p) const { return values_[static_cast<int32_t>(p)]; }

  static float blep(double t, double dt) {
    if (t < dt) { t /= dt; return static_cast<float>(t + t - t * t - 1.0); }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return static_cast<float>(t * t + t + t + 1.0); }
    return 0.0f;
  }
  static float squareBlep(double phase, double dt) {
    double t2 = phase + 0.5;
    t2 -= std::floor(t2);
    return (phase < 0.5 ? 1.0f : -1.0f) + blep(phase, dt) - blep(t2, dt);
  }

  float values_[kHiHatParamCount] = {};
  PercCore core_;
  PercEnv bodyEnv_, chickEnv_;
  PercNoise noise_;
  PercBandpass band_;
  BiquadFilter highpass_{48000.0};
  double phase_[kOsc] = {};
};

}  // namespace webdsp
