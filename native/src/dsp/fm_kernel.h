#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Shared FM building blocks for the Digital Machines (instruments/digital_machine.h and
// instruments/dm_*.h). Docs: docs/digital-machines.md.
//
// Deliberately not a modular graph: these are the pieces two or more Machines needed verbatim
// (a table sine, a phase-modulation operator, exponential envelopes, a state-variable filter,
// "magnetic" ratio mapping, a bandwidth guard, a drive stage). Each Machine still owns its wiring.
//
// All state here is plain data; nothing allocates after construction. The sine table is a
// read-only singleton built on first use (configure(), the message thread), never in render.
namespace webdsp::fmk {

inline constexpr double kTwoPi = 6.283185307179586;
inline constexpr double kInvTwoPi = 1.0 / kTwoPi;

// --- table sine --------------------------------------------------------------------------------

inline constexpr int32_t kTableBits = 11;
inline constexpr int32_t kTableSize = 1 << kTableBits;  // 2048; linear interpolation error ~1.2e-6

class SineTable {
 public:
  SineTable() {
    for (int32_t i = 0; i <= kTableSize; i++) {
      t_[i] = static_cast<float>(std::sin(kTwoPi * static_cast<double>(i) / kTableSize));
    }
  }
  const float* data() const { return t_; }

 private:
  float t_[kTableSize + 1];  // [kTableSize] duplicates [0] so the interpolation never wraps
};

// Built on first call; call it from configure() so render() only ever reads it.
inline const float* sineTable() {
  static const SineTable table;
  return table.data();
}

// sin(2 pi x) for any real x (x in turns).
inline float sinTurns(const float* table, double turns) {
  const double p = turns - std::floor(turns);
  const double x = p * kTableSize;
  const int32_t i = static_cast<int32_t>(x) & (kTableSize - 1);  // p can round to exactly 1.0
  const float f = static_cast<float>(x - std::floor(x));
  return table[i] + f * (table[i + 1] - table[i]);
}

// One phase-modulation operator: a phase accumulator read through the sine table. `pm` is the
// phase offset in radians (index x modulator output).
struct FmOp {
  double phase = 0.0;  // turns, 0..1
  void advance(double inc) {
    phase += inc;
    phase -= std::floor(phase);
  }
  float at(const float* table, float pm) const {
    return sinTurns(table, phase + static_cast<double>(pm) * kInvTwoPi);
  }
};

// --- bandwidth ---------------------------------------------------------------------------------

// Carson-style limit: a modulator at `modHz` with index I spreads sidebands to about
// (I + 1) x modHz; keep that below 0.4 fs. Returns the largest index that stays clean.
inline float maxIndex(double modHz, double sampleRate) {
  return static_cast<float>(std::max(0.0, 0.4 * sampleRate / std::max(modHz, 1.0) - 1.0));
}

// Fades an operator out as its own frequency approaches Nyquist (a carrier there would alias
// outright): 1 below 0.40 fs, 0 above 0.47 fs.
inline float nyquistGain(double hz, double sampleRate) {
  return static_cast<float>(std::clamp((0.47 * sampleRate - hz) / (0.07 * sampleRate), 0.0, 1.0));
}

// --- magnetic ratios ---------------------------------------------------------------------------

// Maps a 0..1 control onto a table of curated ratios. Around each table entry the control sits on
// a plateau (30% of the interval either side, so 60% of the travel is exactly the curated ratio);
// in between it glides geometrically. Sweet spots to rest on, out-of-tune clang to sweep through.
struct MagneticPos {
  int32_t i;  // lower table entry
  float t;    // 0..1 blend toward entry i + 1 (already shaped by the plateau)
};

inline MagneticPos magneticPos(float x, int32_t count) {
  const float pos = std::clamp(x, 0.0f, 1.0f) * static_cast<float>(count - 1);
  const int32_t i = std::min(static_cast<int32_t>(pos), count - 2);
  const float f = pos - static_cast<float>(i);
  const float u = std::clamp((f - 0.3f) / 0.4f, 0.0f, 1.0f);
  return {i, u * u * (3.0f - 2.0f * u)};
}

template <size_t N>
inline float magneticRatio(const float (&table)[N], float x) {
  const MagneticPos m = magneticPos(x, static_cast<int32_t>(N));
  return table[m.i] * std::pow(table[m.i + 1] / table[m.i], m.t);
}

// --- envelopes ---------------------------------------------------------------------------------

// Exponential envelope: short linear attack, one-pole decay toward `sustain` (0 = a pure decay
// that ends itself), one-pole release. trigger() climbs from the *current* level, so a retrigger
// is continuous. `decayCoef`/`releaseCoef` are per-sample multipliers (percDecayCoef).
class ExpEnv {
 public:
  enum class Stage : int32_t { Idle, Attack, Decay, Release };

  void configure(double sampleRate) { sampleRate_ = sampleRate; reset(); }
  void reset() { stage_ = Stage::Idle; level_ = 0.0f; }
  void trigger(float attackSeconds, float decayCoef, float sustain) {
    coef_ = decayCoef;
    sustain_ = std::clamp(sustain, 0.0f, 1.0f);
    step_ = static_cast<float>(1.0 / std::max(static_cast<double>(attackSeconds) * sampleRate_, 1.0));
    stage_ = Stage::Attack;
  }
  // Jumps to `level` and decays from there (a pitch envelope that must be at full depth on the very
  // first sample; trigger() would climb to it over the attack).
  void start(float level, float decayCoef, float sustain) {
    level_ = level;
    coef_ = decayCoef;
    sustain_ = std::clamp(sustain, 0.0f, 1.0f);
    stage_ = Stage::Decay;
  }
  void release(float releaseCoef) {
    if (stage_ == Stage::Idle) return;
    releaseCoef_ = releaseCoef;
    stage_ = Stage::Release;
  }
  float next() {
    switch (stage_) {
      case Stage::Idle: return 0.0f;
      case Stage::Attack:
        level_ += step_;
        if (level_ >= 1.0f) { level_ = 1.0f; stage_ = Stage::Decay; }
        break;
      case Stage::Decay:
        level_ = sustain_ + (level_ - sustain_) * coef_;
        if (sustain_ <= 0.0f && level_ < kFloor) { level_ = 0.0f; stage_ = Stage::Idle; }
        break;
      case Stage::Release:
        level_ *= releaseCoef_;
        if (level_ < kFloor) { level_ = 0.0f; stage_ = Stage::Idle; }
        break;
    }
    return level_;
  }
  bool idle() const { return stage_ == Stage::Idle; }
  float level() const { return level_; }

 private:
  static constexpr float kFloor = 1e-4f;
  double sampleRate_ = 48000.0;
  Stage stage_ = Stage::Idle;
  float level_ = 0.0f, coef_ = 0.0f, releaseCoef_ = 0.0f, sustain_ = 0.0f, step_ = 0.0f;
};

// --- filter ------------------------------------------------------------------------------------

// Trapezoidal state-variable filter (Zavalishin): stable under per-block cutoff changes, all three
// responses from one pass. bp is normalised to unity peak gain.
class Svf {
 public:
  void set(double hz, double q, double sampleRate) {
    g_ = static_cast<float>(std::tan(kTwoPi * 0.5 * std::clamp(hz, 20.0, sampleRate * 0.45) / sampleRate));
    k_ = static_cast<float>(1.0 / std::max(q, 0.1));
    a1_ = 1.0f / (1.0f + g_ * (g_ + k_));
    a2_ = g_ * a1_;
    a3_ = g_ * a2_;
  }
  void reset() { ic1_ = ic2_ = 0.0f; }
  void process(float x, float& lp, float& bp, float& hp) {
    const float v3 = x - ic2_;
    const float v1 = a1_ * ic1_ + a2_ * v3;
    const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
    ic1_ = 2.0f * v1 - ic1_;
    ic2_ = 2.0f * v2 - ic2_;
    lp = v2;
    bp = k_ * v1;
    hp = x - k_ * v1 - v2;
  }

 private:
  float g_ = 0.1f, k_ = 1.0f, a1_ = 0.9f, a2_ = 0.09f, a3_ = 0.009f;
  float ic1_ = 0.0f, ic2_ = 0.0f;
};

// --- shaping -----------------------------------------------------------------------------------

// Rational tanh, exact to ~0.2%: transparent in the middle, bounded by exactly 1.
inline float fastTanh(float x) {
  x = std::clamp(x, -3.0f, 3.0f);
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// Drive: gain, a little asymmetry (even harmonics), level compensated so a 0.7 peak stays near
// 0.7. drive 0 is a bit-exact bypass. Set at control rate, process per sample.
class Drive {
 public:
  void set(float drive) {
    mix_ = std::min(1.0f, drive * 3.0f);
    g_ = 1.0f + 8.0f * drive * drive;
    bias_ = 0.35f * drive;
    off_ = fastTanh(bias_);
    comp_ = 0.7f / fastTanh(g_ * 0.7f);
  }
  float process(float x) const {
    if (mix_ <= 0.0f) return x;
    const float wet = (fastTanh(g_ * x + bias_) - off_) * comp_;
    return x + (wet - x) * mix_;
  }

 private:
  float mix_ = 0.0f, g_ = 1.0f, bias_ = 0.0f, off_ = 0.0f, comp_ = 1.0f;
};

// Transparent below 0.6, then a smooth knee bounded by exactly 1 (as Bass / Fm).
inline float softClip(float x) {
  const float a = std::fabs(x);
  if (a <= 0.6f) return x;
  const float u = std::min((a - 0.6f) * 2.5f, 3.0f);
  const float y = 0.6f + 0.4f * (u * (27.0f + u * u) / (27.0f + 9.0f * u * u));
  const float c = std::min(y, 1.0f);
  return x < 0.0f ? -c : c;
}

// Velocity -> brightness: harder = more sidebands (the level has its own linear scaling).
inline float velocityBrightness(float velocity) { return 0.55f + 0.45f * std::min(velocity, 1.0f); }

}  // namespace webdsp::fmk
