#pragma once
#include "../dsp/percussion.h"

// Synthesized hand clap: several staggered noise bursts followed by a longer tail — the
// "multiple hands landing slightly apart" structure lives *inside* the instrument, so one
// note is one clap. Overview: docs/percussion-instruments.md.
//
//   noise -> band-pass (Tune, Q from Snap) + low band-pass (room/body) -> burst env -> tanh
//   burst env: 3 short bursts at 0, d, 2d (ms-scale) then the Decay-length tail at 3d,
//   d = Spread-scaled; each burst re-attacks from the current level and has its own gain.
namespace webdsp {

inline constexpr InstrumentParamInfo kClapParams[] = {
    {static_cast<int32_t>(ClapParam::Tune), "tune", 600.0f, 3000.0f, 1300.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(ClapParam::Decay), "decay", 0.05f, 0.8f, 0.25f, "s", ModDomain::Log},
    {static_cast<int32_t>(ClapParam::Spread), "spread", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(ClapParam::Snap), "snap", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
};
inline constexpr int32_t kClapParamCount =
    static_cast<int32_t>(sizeof(kClapParams) / sizeof(kClapParams[0]));

class ClapInstrument final : public Instrument {
 public:
  ClapInstrument() {
    for (int32_t i = 0; i < kClapParamCount; i++) values_[i] = kClapParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Clap; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    env_.configure(sampleRate);
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    if (core_.begin(velocity, delayFrames, pan, pitch)) {
      bandHi_.reset();
      bandLo_.reset();
    }
    noise_.reset();
    // Burst k fires at k * spacing (spacing 4..16 ms across Spread); burst 0 fires right away.
    const double spacingSec = 0.004 + 0.012 * value(ClapParam::Spread);
    spacingFrames_ = std::max<int32_t>(1, static_cast<int32_t>(spacingSec * core_.sampleRate));
    burst_ = 0;
    sinceBurst_ = 0;
    triggerBurst();
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    burst_ = kBursts;  // no more bursts
    env_.setDecay(percDecayCoef(0.004f, core_.sampleRate));
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kClapParams, kClapParamCount, values_, param, v);
  }

  void reset() override {
    env_.reset();
    bandHi_.reset();
    bandLo_.reset();
    burst_ = kBursts;
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* mono = scratch[0];
    const double sr = core_.sampleRate;
    const float snap = value(ClapParam::Snap);

    const double centre = value(ClapParam::Tune) * core_.pitchRatio;
    bandHi_.set(centre, 1.0 + 2.5 * snap, sr);
    bandLo_.set(centre * 0.45, 1.2, sr);
    const float gain = core_.velocity * kLevel;

    bool finished = false;
    for (int32_t i = 0; i < numFrames; i++) {
      if (core_.pendingDelay > 0) {
        core_.pendingDelay--;
        mono[i] = 0.0f;
        continue;
      }
      if (burst_ < kBursts && ++sinceBurst_ >= spacingFrames_) {
        sinceBurst_ = 0;
        triggerBurst();
      }
      if (!finished && burst_ >= kBursts && env_.done()) finished = true;
      if (finished) { mono[i] = 0.0f; continue; }

      const float n = noise_.next();
      const float s = bandHi_.process(n) + 0.5f * bandLo_.process(n);
      mono[i] = std::tanh(2.0f * s * env_.next() * burstGain_) * gain;
    }
    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr int32_t kBursts = 4;  // 3 short hits + the tail
  static constexpr float kLevel = 0.85f;
  float value(ClapParam p) const { return values_[static_cast<int32_t>(p)]; }

  void triggerBurst() {
    // Slightly uneven hits; the last (tail) burst is the loudest and the only long one.
    static constexpr float kGains[kBursts] = {0.9f, 0.75f, 0.9f, 1.0f};
    const bool tail = burst_ == kBursts - 1;
    const float seconds = tail ? value(ClapParam::Decay) * kDecayToTau : 0.005f;
    env_.trigger(percDecayCoef(seconds, core_.sampleRate), 0.0003f);
    burstGain_ = kGains[burst_];
    burst_++;
  }

  float values_[kClapParamCount] = {};
  PercCore core_;
  PercEnv env_;
  PercNoise noise_;
  PercBandpass bandHi_, bandLo_;
  int32_t burst_ = kBursts, sinceBurst_ = 0, spacingFrames_ = 1;
  float burstGain_ = 1.0f;
};

}  // namespace webdsp
