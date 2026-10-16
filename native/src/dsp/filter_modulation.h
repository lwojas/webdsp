#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "biquad_filter.h"

// Internal filter-cutoff modulation, shared by synth instruments. Not a modulation system:
// there is no source, route or destination type. The instrument owns its modulation signal (a
// timbre envelope, the Pulse repeat contour, a future slow movement) and says "apply this much
// of it"; this class only does the filter-side mechanics.
//
//   effective = clamp(max(base * 2^(depthOctaves * signal), floorHz), kMinHz, maxHz)
//
// - `base`: the resting cutoff in Hz, after whatever the instrument applies to it (keytrack).
// - `signal`: the instrument's modulation value in [-1, +1] (clamped). Unipolar sources simply
//   use 0..1; negative values close the filter below base. 0 leaves the cutoff at base exactly.
// - `depthOctaves`: how far a signal of 1 moves the cutoff. Octaves, not Hz, so the same depth
//   sounds alike at 200 Hz and at 2 kHz and negative movement can never cross zero.
// - `floorHz`: instrument-specific lower bound (Bass/Pulse use 0.9 x the note's fundamental so
//   the filter can darken a note but never delete it). Pass 0 for none.
//
// Update granularity and smoothing are the caller's control rate (Bass and Pulse call `apply`
// once per 16-sample sub-block) plus a 0.2% change threshold before the biquad's coefficients are
// recomputed; there is no second smoother. The one addition is a safety rate limit: `apply` moves
// the cutoff by at most kMaxOctavesPerApply per call. A transposed-DF2 biquad whose coefficients
// jump across many octaves between 16-sample blocks (a step or square-wave source, high Q) can
// blow up (measured: +-2 octaves toggling every block at Q 0.7 diverges to inf). The limit is set
// just above the fastest move the shipped instruments make (Bass's 1 ms attack at bite 1 moves
// ~1.3 oct per block; Pulse's repeat <= ~0.1), so their output is unchanged (<= 1 LSB of int16
// vs. before) and it only acts on abrupt sources. With it, a full-range square-wave source at Q 9.7
// stays finite and bounded (resonance gain, peak ~76 in the worst measured case, no divergence). The first call after `reset` jumps
// straight to the target (fresh filter, nothing to protect). Cheap: one exp2 (two when limited)
// per call, no allocation.
namespace webdsp {

class FilterModulation {
 public:
  static constexpr float kMinHz = 30.0f;
  static constexpr float kMaxHzAbsolute = 18000.0f;
  static constexpr float kMaxNyquistFraction = 0.45f;
  static constexpr float kChangeThreshold = 0.002f;  // relative
  static constexpr float kMaxOctavesPerApply = 1.5f;

  void configure(double sampleRate) {
    maxHz_ = static_cast<float>(std::min<double>(kMaxHzAbsolute, sampleRate * kMaxNyquistFraction));
    reset();
  }

  // Forget the last cutoff sent to the filter so the next `apply` always writes.
  void reset() { lastHz_ = -1.0f; }

  // The pure cutoff calculation; always returns a finite value in [kMinHz, maxHz].
  float cutoffHz(float baseHz, float depthOctaves, float signal, float floorHz = 0.0f) const {
    float s = std::clamp(signal, -1.0f, 1.0f);
    float octs = depthOctaves * s;
    if (!std::isfinite(octs)) octs = 0.0f;
    float hz = baseHz * std::exp2(octs);
    if (!std::isfinite(hz)) hz = baseHz;
    return std::clamp(std::max(hz, floorHz), kMinHz, maxHz_);
  }

  // Computes the target cutoff, limits its movement from the last applied value to
  // kMaxOctavesPerApply, and sends the result to `filter` if it moved by more than the
  // threshold. Returns the cutoff in effect.
  float apply(BiquadFilter& filter, float baseHz, float depthOctaves, float signal,
              float floorHz = 0.0f) {
    float hz = cutoffHz(baseHz, depthOctaves, signal, floorHz);
    if (lastHz_ > 0.0f) {
      const float step = std::clamp(std::log2(hz / lastHz_), -kMaxOctavesPerApply, kMaxOctavesPerApply);
      hz = std::clamp(lastHz_ * std::exp2(step), kMinHz, maxHz_);
    }
    if (std::fabs(hz - lastHz_) > kChangeThreshold * hz) {
      filter.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), hz);
      lastHz_ = hz;
    }
    return hz;
  }

  float maxHz() const { return maxHz_; }

 private:
  float maxHz_ = 18000.0f;
  float lastHz_ = -1.0f;
};

}  // namespace webdsp
