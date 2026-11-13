#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Harmonic oscillator: a sine whose harmonic content is one control. Used by the Sub Bass instrument
// (instruments/subbass.h); docs/subbass-instrument.md. Not a wavetable engine: there are no tables and nothing
// to load. The spectrum is a function of (profile, shape) evaluated at the control rate, and the waveform is
// summed from it, so the oscillator is deterministic, allocation-free and exactly band-limited.
//
//   y(phase) = g * sum_{n=1..H} a_n sin(2 pi n phase)         a_1 = 1 (before normalisation)
//   a_n      = n^-tilt(shape) * parity(n) * clamp(Heff - (n - 1), 0, 1)
//   Heff     = min( 1 + (maxH - 1) * shape^curve ,  band limit )
//
// * Shape 0 gives Heff = 1: every harmonic weight is exactly zero and the output is one std::sin. A genuinely
//   clean sine, not "a very quiet shaper".
// * Raising shape opens harmonics one at a time (a harmonic fades in over one unit of Heff) while the spectral
//   tilt flattens, so the timbre goes sine -> rounded -> strong 2nd/3rd -> bright -> saw-like, with no steps.
// * The band limit is part of Heff: harmonic n is only audible while n * f0 < 0.42 fs, and it fades out
//   across one harmonic as pitch rises. A pitch envelope or glide therefore never aliases and never clicks a
//   harmonic in or out.
// * Every harmonic is a sine at phase 0, so the wave is odd-symmetric and has no DC at any shape.
// * Output is normalised to the RMS of a unit sine (0.7071), so the fundamental stays near full level while the
//   shape adds harmonics and the loudness moves little (a saw-like wave has its fundamental ~2 dB down).
//
// Profiles change where the shape knob goes, not what it is: Classic (the all-rounder), Warm (2nd/3rd-heavy,
// stops early), Hollow (odd harmonics only), Bright (slow roll-off), Saw (full 1/n quickly).
//
// The harmonic sum uses the Chebyshev recurrence s_{n+1} = 2 cos(t) s_n - s_{n-1} in double, so a note costs one
// sin and one cos per sample plus ~H multiply-adds (H <= 64).
namespace webdsp {

inline constexpr int32_t kHarmonicProfileCount = 5;
inline constexpr int32_t kMaxHarmonics = 64;
inline constexpr float kHarmonicBandLimit = 0.42f;  // highest harmonic stays below this x the sample rate

struct HarmonicProfileDef {
  float tiltLo, tiltHi;  // exponent of the 1/n roll-off at shape 0 and 1 (smaller = brighter)
  float even, odd;       // gain of even / odd harmonics
  float maxH;            // harmonics available at shape 1 (before the band limit)
  float curve;           // how late the harmonics open: Heff = 1 + (maxH - 1) shape^curve
};

inline constexpr HarmonicProfileDef kHarmonicProfiles[kHarmonicProfileCount] = {
    {2.0f, 1.0f, 1.0f, 1.0f, 48.0f, 3.0f},  // 0 Classic
    {2.4f, 1.4f, 1.3f, 0.75f, 12.0f, 2.0f}, // 1 Warm
    {1.6f, 1.0f, 0.0f, 1.0f, 48.0f, 3.0f},  // 2 Hollow
    {1.3f, 0.6f, 1.0f, 1.0f, 64.0f, 2.5f},  // 3 Bright
    {1.0f, 1.0f, 1.0f, 1.0f, 48.0f, 2.0f},  // 4 Saw
};
inline constexpr const char* kHarmonicProfileNames[kHarmonicProfileCount] = {"classic", "warm", "hollow", "bright", "saw"};

// Harmonic amplitudes for a profile and shape. `bandLimit` is the most harmonics the pitch allows (see
// harmonicBandLimit()); pass a huge value for the ideal spectrum. Fills amp[1..kMaxHarmonics] (amp[0] unused),
// normalised as described above; returns the highest harmonic with a nonzero weight (>= 1). Pure function.
inline int32_t harmonicSpectrum(int32_t profile, float shape, float bandLimit, float* amp) {
  const HarmonicProfileDef& p = kHarmonicProfiles[std::clamp(profile, 0, kHarmonicProfileCount - 1)];
  const float s = std::clamp(std::isfinite(shape) ? shape : 0.0f, 0.0f, 1.0f);
  const float tilt = p.tiltLo + (p.tiltHi - p.tiltLo) * s;
  const float heff = std::max(1.0f, std::min(1.0f + (p.maxH - 1.0f) * std::pow(s, p.curve), bandLimit));
  const int32_t top = std::min(static_cast<int32_t>(std::ceil(heff)), kMaxHarmonics);
  amp[0] = 0.0f;
  amp[1] = 1.0f;
  double energy = 1.0;
  for (int32_t n = 2; n <= kMaxHarmonics; n++) {
    float a = 0.0f;
    if (n <= top) {
      const float open = std::clamp(heff - static_cast<float>(n - 1), 0.0f, 1.0f);
      a = std::pow(static_cast<float>(n), -tilt) * ((n & 1) ? p.odd : p.even) * open;
    }
    amp[n] = a;
    energy += static_cast<double>(a) * a;
  }
  const float g = static_cast<float>(1.0 / std::sqrt(energy));
  for (int32_t n = 1; n <= kMaxHarmonics; n++) amp[n] *= g;
  int32_t highest = 1;
  for (int32_t n = kMaxHarmonics; n >= 2; n--) {
    if (amp[n] != 0.0f) { highest = n; break; }
  }
  return highest;
}

class HarmonicOsc {
 public:
  void reset(double phaseTurns = 0.0) { phase_ = phaseTurns - std::floor(phaseTurns); }
  double phase() const { return phase_; }

  // Compute the spectrum for the next stretch of samples. `maxInc` is the largest phase increment (f0 / fs) the
  // stretch will use, which sets the band limit (`bandFraction` x the sample rate, kHarmonicBandLimit by default;
  // a caller that follows the oscillator with a nonlinearity lowers it to leave room for the new harmonics).
  // The amplitudes glide linearly to the new spectrum over `rampSamples` next() calls, so a control step never
  // makes a staircase (which would splatter into the top octave); 1 = jump. The first call snaps.
  void setSpectrum(int32_t profile, float shape, double maxInc, int32_t rampSamples = 1,
                   float bandFraction = kHarmonicBandLimit) {
    const float limit = std::max(1.0f, static_cast<float>(bandFraction / std::max(maxInc, 1e-9)) - 1.0f);
    tgtTop_ = harmonicSpectrum(profile, shape, limit, tgt_);
    if (!primed_ || rampSamples <= 1) {
      snap();
      primed_ = true;
      return;
    }
    const float inv = 1.0f / static_cast<float>(rampSamples);
    const int32_t span = std::max(tgtTop_, curTop_);
    for (int32_t n = 1; n <= span; n++) step_[n] = (tgt_[n] - cur_[n]) * inv;
    remaining_ = rampSamples;
    curTop_ = span;
  }

  // Jump straight to the target spectrum.
  void snap() {
    for (int32_t n = 1; n <= kMaxHarmonics; n++) cur_[n] = tgt_[n];
    curTop_ = tgtTop_;
    remaining_ = 0;
  }

  // Highest harmonic currently in use (1 = pure sine).
  int32_t harmonics() const { return curTop_; }
  float amplitude(int32_t n) const { return cur_[std::clamp(n, 1, kMaxHarmonics)]; }

  // Advance by `inc` turns and return the sample at the new phase.
  float next(double inc) {
    phase_ += inc;
    phase_ -= std::floor(phase_);
    if (remaining_ > 0) {
      for (int32_t n = 1; n <= curTop_; n++) cur_[n] += step_[n];
      if (--remaining_ == 0) snap();
    }
    return sampleAt(phase_);
  }

  float sampleAt(double turns) const {
    const double t = 6.283185307179586 * (turns - std::floor(turns));
    const double s1 = std::sin(t);
    if (curTop_ <= 1) return static_cast<float>(s1 * cur_[1]);  // shape 0: exactly a sine
    const double c2 = 2.0 * std::cos(t);
    double sPrev = 0.0, sCur = s1;
    double sum = static_cast<double>(cur_[1]) * s1;
    for (int32_t n = 2; n <= curTop_; n++) {
      const double sNext = c2 * sCur - sPrev;
      sPrev = sCur;
      sCur = sNext;
      sum += static_cast<double>(cur_[n]) * sCur;
    }
    return static_cast<float>(sum);
  }

 private:
  double phase_ = 0.0;
  int32_t tgtTop_ = 1, curTop_ = 1, remaining_ = 0;
  bool primed_ = false;
  float tgt_[kMaxHarmonics + 1] = {0.0f, 1.0f};
  float cur_[kMaxHarmonics + 1] = {0.0f, 1.0f};
  float step_[kMaxHarmonics + 1] = {};
};

}  // namespace webdsp
