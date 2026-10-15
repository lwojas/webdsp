#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../instrument.h"
#include "biquad_filter.h"
#include "panner.h"

// Small building blocks shared by the percussion instruments (instruments/snare.h, clap.h,
// tom.h, hihat.h). Deliberately not a drum-synth framework: each instrument still owns its
// signal path; these are only the pieces that three or more of them needed verbatim.
namespace webdsp {

// "Decay" on every percussion instrument is roughly the time to fall ~40 dB, so a one-pole
// time constant is decay * kDecayToTau (4.6 time constants = 40 dB).
inline constexpr float kDecayToTau = 0.217f;

// Per-sample multiplier of a one-pole exponential decay with time constant `seconds`.
inline float percDecayCoef(float seconds, double sampleRate) {
  return static_cast<float>(
      std::exp(-1.0 / (std::max(static_cast<double>(seconds), 1e-5) * sampleRate)));
}

// Deterministic white noise in -1..1 (xorshift32). reset() on every note => identical hits.
struct PercNoise {
  static constexpr uint32_t kSeed = 0x9E3779B9u;
  uint32_t s = kSeed;
  void reset() { s = kSeed; }
  float next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return static_cast<float>(static_cast<int32_t>(s)) * (1.0f / 2147483648.0f);
  }
};

// Exponential decay envelope with a short linear attack. trigger() always climbs from the
// *current* level, so a retrigger is continuous (no click) instead of snapping to 0.
class PercEnv {
 public:
  void configure(double sampleRate) { sampleRate_ = sampleRate; reset(); }
  void reset() { level_ = 0.0f; attacking_ = false; }
  void trigger(float decayCoef, float attackSeconds = 0.0005f) {
    coef_ = decayCoef;
    step_ = static_cast<float>(1.0 / std::max(attackSeconds * sampleRate_, 1.0));
    attacking_ = true;
  }
  void setDecay(float decayCoef) { coef_ = decayCoef; }
  float next() {
    if (attacking_) {
      level_ += step_;
      if (level_ >= 1.0f) { level_ = 1.0f; attacking_ = false; }
    } else {
      level_ *= coef_;
    }
    return level_;
  }
  bool done() const { return !attacking_ && level_ < 1e-4f; }
  float level() const { return level_; }

 private:
  double sampleRate_ = 48000.0;
  float level_ = 0.0f, coef_ = 0.0f, step_ = 0.0f;
  bool attacking_ = false;
};

// Constant-peak-gain two-pole band-pass (RBJ). Per-sample so it can sit inside a render loop;
// coefficients are cheap to retune, even while running.
class PercBandpass {
 public:
  void set(double hz, double q, double sampleRate) {
    const double w = 6.283185307179586 * std::clamp(hz, 20.0, sampleRate * 0.45) / sampleRate;
    const double alpha = std::sin(w) / (2.0 * std::max(q, 0.1));
    const double a0 = 1.0 + alpha;
    b0_ = alpha / a0;
    a1_ = -2.0 * std::cos(w) / a0;
    a2_ = (1.0 - alpha) / a0;
  }
  void reset() { z1_ = z2_ = 0.0; }
  float process(float x) {
    const double y = b0_ * x + z1_;
    z1_ = z2_ - a1_ * y;            // b1 = 0
    z2_ = -b0_ * x - a2_ * y;       // b2 = -b0
    return static_cast<float>(y);
  }

 private:
  double b0_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

// Note-lifecycle state every one-shot drum carries identically (velocity, pitch offset, pan,
// pre-note delay, active/releasing). Instruments compose this; they do not inherit it.
struct PercCore {
  double sampleRate = 48000.0;
  float velocity = 1.0f;
  double pitchRatio = 1.0;  // 2^(note pitch / 12): multiplies the instrument's own Tune
  bool active = false;
  bool releasing = false;
  int32_t pendingDelay = 0;
  Panner panner;

  void configure(double sr) {
    sampleRate = sr;
    panner.configure(sr);
    clear();
  }
  // Returns true when this note starts a sound from silence (vs. retriggering a live one).
  bool begin(float vel, int32_t delayFrames, float pan, float pitchSemitones) {
    velocity = std::isfinite(vel) ? std::max(0.0f, vel) : 0.0f;
    pendingDelay = std::max(0, delayFrames);
    panner.setPan(std::isfinite(pan) ? pan : 0.0f);
    pitchRatio = std::isfinite(pitchSemitones)
                     ? std::exp2(std::clamp(pitchSemitones, -48.0f, 48.0f) / 12.0)
                     : 1.0;
    const bool fresh = !active;
    if (fresh) panner.snap(true);
    active = true;
    releasing = false;
    return fresh;
  }
  void clear() { active = false; releasing = false; pendingDelay = 0; }

  // Accumulates a mono buffer into the bus (panned when stereo).
  void route(float* mono, float* const* busChannels, int32_t busChannelCount, int32_t numFrames) {
    const int32_t channels = std::min(busChannelCount, kMaxDspChannels);
    if (channels >= 2 && !panner.isUnity()) {
      panner.process(mono, mono, busChannels[0], busChannels[1], numFrames, true, true);
    } else {
      for (int32_t c = 0; c < channels; c++) {
        for (int32_t i = 0; i < numFrames; i++) busChannels[c][i] += mono[i];
      }
    }
  }
};

// Clamped write into a values_[] array indexed like `table`. Returns false for an unknown id.
inline bool setTableParam(const InstrumentParamInfo* table, int32_t count, float* values,
                          int32_t param, float v) {
  if (!std::isfinite(v)) return true;
  for (int32_t i = 0; i < count; i++) {
    if (table[i].id != param) continue;
    values[i] = std::clamp(v, table[i].min, table[i].max);
    return true;
  }
  return false;
}

}  // namespace webdsp
