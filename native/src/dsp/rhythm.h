#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Rhythmic timing and shaping primitives for the Pulse instrument (instruments/pulse.h; design:
// docs/pulse-instrument-proposal.md). Pure functions of phase — no state, no allocation — so each
// curve is testable on its own. "Phase" is the position inside one rhythmic cycle, 0..1, derived
// from the engine's beat grid (Engine transport), not from when a note started.
namespace webdsp {
namespace rhythm {

// Rate steps are note values of a 4/4 bar: 0 = 1/1 (four beats) ... 5 = 1/32.
inline constexpr int32_t kRateSteps = 6;

inline int32_t rateStep(float value) {
  if (!std::isfinite(value)) return 2;
  return std::clamp(static_cast<int32_t>(std::lround(value)), 0, kRateSteps - 1);
}

// Frames in one cycle of `step` at the given tempo (`framesPerBeat`, a quarter note).
inline double periodFrames(double framesPerBeat, int32_t step) {
  return framesPerBeat * 4.0 / static_cast<double>(1 << std::clamp(step, 0, kRateSteps - 1));
}

// Phase 0..1 of absolute frame `frame` on the grid anchored at `anchorFrame`. Frames before the
// anchor wrap the same way, so a grid extends backwards seamlessly.
inline double phaseAt(double frame, double anchorFrame, double period) {
  const double x = (frame - anchorFrame) / std::max(period, 1.0);
  return x - std::floor(x);
}

// --- Pumper ---------------------------------------------------------------------------------
// Fraction of the cycle the duck takes to recover. More pump = deeper *and* longer.
// Depth is set in dB, because loudness is: amount 0.5 is a clear -15 dB pump, 1.0 a -30 dB near-gate
// (never a full gate: the note stays audible). A linear gain depth made the middle of the knob a
// shallow, short dip that did not read as sidechain ducking.
inline constexpr float kPumpMaxDb = 30.0f;

inline float pumpRecovery(float amount) { return 0.35f + 0.5f * std::clamp(amount, 0.0f, 1.0f); }

// Duck shape at phase `p`: 1 at the hit (p = 0), falling to 0 at `recovery`, then 0 until the
// cycle ends. Squared, so recovery is quick at first and eases in (a pump, not a ramp).
inline float pumpDuck(float p, float recovery) {
  if (p >= recovery) return 0.0f;
  const float u = 1.0f - p / recovery;
  return u * u;
}

// Gain -kPumpMaxDb * amount * duck, in dB. amount 0 => exactly 1 for every phase, and so is any
// phase past the recovery (duck is exactly 0 there).
inline float pumpGain(float p, float amount) {
  const float a = std::clamp(amount, 0.0f, 1.0f);
  const float db = -kPumpMaxDb * a * pumpDuck(p, pumpRecovery(a));
  return std::exp2(db * 0.16609640474f);  // 10^(db/20) = 2^(db * log2(10)/20)
}

// --- Repeater -------------------------------------------------------------------------------
// Unipolar contour 0..1: zero at the hit, peaks (1) at p = kRepeatPeak as the pump is recovering,
// then falls smoothly back to exactly 0 at the cycle end, so it wraps without a step.
inline constexpr float kRepeatPeak = 0.2f;

inline float repeatContour(float p) {
  const float x = p / kRepeatPeak;
  const float g = x * std::exp(1.0f - x);
  // g(1/peak) is the tail value at the cycle end; subtract a ramp to land on exactly 0.
  constexpr float kEnd = 1.0f / kRepeatPeak;
  const float tail = kEnd * 0.01831563889f;  // e^(1 - 1/peak) = e^-4
  const float v = (g - tail * p) / (1.0f - tail * kRepeatPeak);
  return std::clamp(v, 0.0f, 1.0f);
}

}  // namespace rhythm
}  // namespace webdsp
