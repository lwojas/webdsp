#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Band-limited additive oscillator for the Edge instrument (instruments/edge.h). Docs:
// docs/edge-instrument.md.
//
// Why not PolyBLEP like the other synths: PolyBLEP corrects a naive waveform around its edges, so
// the spectrum is "an ideal saw, minus aliasing" and its brightness can only be changed by a
// filter. Here the spectrum is *constructed*: a set of single-cycle tables holds a falling saw
// built from exactly H harmonics (H = 1, 2, 3, 4, 6, 8, 11, 16 ... 256, Lanczos-windowed so the
// brick-wall edge rings less). Choosing the table chooses how many harmonics the oscillator has,
// so brightness is an oscillator control: it can be swept by an envelope with no filter and no
// resonance, it is exactly alias-free up to the Nyquist cap, and the result is cleaner and more
// "digital" than a subtractive oscillator (a pure sine at one end, a 256-partial saw at the other).
//
// Waveform morph: the shape is `S(p) - k * S(p + w)`, the same band-limited saw read twice. k = 0 is
// a saw; k = 1, w = 0.5 is a square; k = 1, w -> 0.12 is a narrow pulse. Both reads share the table
// choice, so the morph is band-limited too. Output is normalised to a constant RMS (each table, and
// the morph at every shape step, computed exactly from the harmonic weights), so sweeping brightness
// or shape changes the timbre, not the loudness.
//
// The table set is a read-only singleton (16 x 2049 floats), built on first use from configure(),
// never in render(). Reading is linear interpolation; no allocation.
namespace webdsp::edge {

inline constexpr int32_t kTableSize = 2048;
inline constexpr int32_t kLevels = 16;
inline constexpr int32_t kMaxHarmonics = 256;
inline constexpr float kTargetRms = 0.45f;
inline constexpr int32_t kShapeSteps = 32;  // resolution of the precomputed morph gains (knob 0..1)
inline constexpr int32_t kLevelHarmonics[kLevels] = {1, 2, 3, 4, 6, 8, 11, 16, 23, 32, 45, 64, 91, 128, 181, 256};

// Shape knob 0..1 -> second-read amount k and phase offset w (turns): 0 saw, 0.5 square, 1 narrow pulse.
inline void shapeParams(float shape, float* k, double* w) {
  const float sh = std::clamp(shape, 0.0f, 1.0f);
  if (sh < 0.5f) {
    *k = sh * 2.0f;
    *w = 0.5;
  } else {
    *k = 1.0f;
    *w = 0.5 - (sh - 0.5) * 2.0 * 0.38;
  }
}

class Tables {
 public:
  Tables() {
    const double twoPi = 6.283185307179586;
    float sine[kTableSize];
    for (int32_t i = 0; i < kTableSize; i++) sine[i] = static_cast<float>(std::sin(twoPi * i / kTableSize));
    for (int32_t j = 0; j < kLevels; j++) {
      const int32_t h = kLevelHarmonics[j];
      float* t = t_[j];
      for (int32_t i = 0; i < kTableSize; i++) t[i] = 0.0f;
      for (int32_t n = 1; n <= h; n++) {
        // Lanczos sigma factor: softens the Gibbs ringing of a hard harmonic cutoff.
        const double x = static_cast<double>(n) / (h + 1);
        const double sigma = std::sin(3.141592653589793 * x) / (3.141592653589793 * x);
        const float amp = static_cast<float>((2.0 / 3.141592653589793) * sigma / n);
        for (int32_t i = 0; i < kTableSize; i++) t[i] += amp * sine[(n * i) & (kTableSize - 1)];
      }
      double e = 0.0;
      for (int32_t i = 0; i < kTableSize; i++) e += static_cast<double>(t[i]) * t[i];
      const float g = static_cast<float>(kTargetRms / std::sqrt(e / kTableSize));
      for (int32_t i = 0; i < kTableSize; i++) t[i] *= g;
      t[kTableSize] = t[0];  // guard point: interpolation never wraps

      // Loudness of the morph at every shape step, from the same harmonic weights: the energy of
      // S(p) - k S(p + w) is sum a_n^2 |1 - k e^(i 2 pi n w)|^2, so a sine (one harmonic) and a full
      // saw need different gains to stay at the same RMS.
      for (int32_t q = 0; q <= kShapeSteps; q++) {
        float k;
        double w;
        shapeParams(static_cast<float>(q) / kShapeSteps, &k, &w);
        double base = 0.0, morph = 0.0;
        for (int32_t n = 1; n <= h; n++) {
          const double x = static_cast<double>(n) / (h + 1);
          const double sigma = std::sin(3.141592653589793 * x) / (3.141592653589793 * x);
          const double a2 = sigma * sigma / (double(n) * n);
          base += a2;
          morph += a2 * (1.0 + double(k) * k - 2.0 * k * std::cos(twoPi * n * w));
        }
        shapeGain_[q][j] = static_cast<float>(std::sqrt(base / std::max(morph, 1e-9)));
      }
    }
  }
  const float* level(int32_t j) const { return t_[j]; }
  // Gain that holds RMS constant at level j for a shape knob (interpolated between steps).
  float shapeGain(int32_t j, float shape) const {
    const float x = std::clamp(shape, 0.0f, 1.0f) * kShapeSteps;
    const int32_t q = std::min(static_cast<int32_t>(x), kShapeSteps - 1);
    const float f = x - q;
    return shapeGain_[q][j] + f * (shapeGain_[q + 1][j] - shapeGain_[q][j]);
  }

 private:
  float t_[kLevels][kTableSize + 1];
  float shapeGain_[kShapeSteps + 1][kLevels];
};

inline const Tables& tables() {
  static const Tables t;
  return t;
}

// Everything a read needs, recomputed at the control rate.
struct Shape {
  const float* lo = nullptr;  // table with fewer harmonics
  const float* hi = nullptr;  // next table up
  float mix = 0.0f;           // 0 = lo only, 1 = hi only
  float k = 0.0f;             // second-read amount (saw -> pulse)
  double w = 0.5;             // second-read phase offset (turns)
  float gain = 1.0f;          // keeps RMS constant across the morph
};

// brightness 0..1 -> harmonic limit 1..256 (octave steps: each 1/8 of the knob doubles it).
inline float harmonicLimit(float brightness) {
  return std::exp2(8.0f * std::clamp(brightness, 0.0f, 1.0f));
}

// `shape` 0..1: 0 saw, 0.5 square, 1 narrow pulse. `incMax` is the largest per-sample phase
// increment the oscillators will see in this control step; harmonics above 0.45 fs are never
// used, so nothing aliases at any brightness (quantised down to a table level).
inline Shape makeShape(const Tables& tb, float brightness, float shape, double incMax) {
  Shape s;
  const double cap = 0.45 / std::max(incMax, 1e-6);
  float target = std::min(harmonicLimit(brightness), static_cast<float>(std::min(cap, double(kMaxHarmonics))));
  target = std::max(target, 1.0f);
  int32_t top = 0;  // highest level that is allowed at this pitch
  for (int32_t j = 0; j < kLevels; j++) if (kLevelHarmonics[j] <= cap) top = j;
  int32_t j = 0;
  while (j + 1 <= top && kLevelHarmonics[j + 1] <= target) j++;
  int32_t loIdx = top, hiIdx = top;
  if (j >= top) {
    s.lo = s.hi = tb.level(top);
    s.mix = 0.0f;
  } else {
    loIdx = j;
    hiIdx = j + 1;
    const float a = std::log2(static_cast<float>(kLevelHarmonics[j]));
    const float b = std::log2(static_cast<float>(kLevelHarmonics[j + 1]));
    s.lo = tb.level(j);
    s.hi = tb.level(j + 1);
    s.mix = std::clamp((std::log2(target) - a) / (b - a), 0.0f, 1.0f);
  }
  shapeParams(shape, &s.k, &s.w);
  s.gain = (1.0f - s.mix) * tb.shapeGain(loIdx, shape) + s.mix * tb.shapeGain(hiIdx, shape);
  return s;
}

inline float readTable(const float* t, double phase) {
  const double x = phase * kTableSize;
  const int32_t i = static_cast<int32_t>(x);
  const float f = static_cast<float>(x - i);
  return t[i] + f * (t[i + 1] - t[i]);
}

// One sample at `phase` (turns, 0 <= phase < 1).
inline float read(const Shape& s, double phase) {
  double p2 = phase + s.w;
  p2 -= std::floor(p2);
  float y = readTable(s.lo, phase);
  if (s.k > 0.0f) y -= s.k * readTable(s.lo, p2);
  if (s.mix > 0.0f) {
    float z = readTable(s.hi, phase);
    if (s.k > 0.0f) z -= s.k * readTable(s.hi, p2);
    y += s.mix * (z - y);
  }
  return y * s.gain;
}

}  // namespace webdsp::edge
