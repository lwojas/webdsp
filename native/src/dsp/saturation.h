#pragma once
#include <algorithm>
#include <cmath>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// A single memoryless waveshaper (tanh soft clip) with pre-gain (drive), asymmetry, output
// gain, and dry/wet mix — the "smallest useful" saturation design per
// docs/effects-algorithm-survey.md, chosen explicitly over a general-purpose distortion
// framework (multiple selectable curves, per-curve parameter sets, an oversampling engine):
// one fixed curve, four scalar params, no extra state beyond a per-channel DC blocker.
//
// Topology: dry sample -> pre-gain (drive, dB) -> tanh(x + asymmetry bias) -> one-pole DC
// blocker -> output gain (dB) -> dry/wet mix. Asymmetry biases the shaper's input before the
// tanh rather than using two different curves for the positive/negative halves — the
// standard cheap way to get the even-harmonic content of an asymmetric (tube/diode-like)
// clipper out of a single odd-symmetric tanh, per the DAFX nonlinear-processing chapter cited
// in the survey. That bias necessarily introduces a DC offset into the shaped signal (an
// asymmetric transfer curve fed a zero-mean input does not itself produce a zero-mean
// output) — the DC blocker removes it unconditionally rather than only when asymmetry != 0,
// since it's cheap (two scalars/channel) and correct at asymmetry == 0 too (a no-op to
// numerical precision, since tanh(drive*x) is already odd-symmetric there).
//
// The DC blocker's own recursive state is clamped to [-1, 1] every sample (same nominal
// range tanh itself is bounded to) — measured empirically while building this node: an
// *unclamped* one-pole DC blocker (the textbook `y[n] = x[n] - x[n-1] + R*y[n-1]`) fed a
// heavily-driven, near-square-wave signal (high drive, low frequency) rings up to ~1.9x the
// input's peak before settling — a real, reachable transient overshoot from this filter
// topology's own impulse response (an L1 norm around 2, not the ~1.0025 steady-state
// sinusoidal gain the frequency response alone would suggest), not a hypothetical edge case.
// Clamping the recursive state directly (rather than only clamping the final output) keeps
// the filter itself from ever accumulating that overshoot in the first place, at the cost of
// a small amount of extra distortion only in this already-extreme corner of the parameter
// space — the standard anti-windup-style fix for a leaky integrator/differentiator, and
// cheaper than adding a second limiter stage.
//
// No oversampling: hard-driven high-frequency content will alias (see docs/saturation-node.md
// for the measured aliasing assessment), the documented and accepted v1 tradeoff per the
// survey's own recommendation. Oversampling remains the upgrade path if that proves
// insufficient, not something this node's shape needs to anticipate.
namespace webdsp {

class Saturation final : public DSPNode {
 public:
  // sampleRate is accepted (unused) purely so Saturation's constructor matches every other
  // DSPNode's shape (Bus constructs all of them uniformly — see bus.h) — a memoryless
  // waveshaper plus a fixed-coefficient DC blocker has no sample-rate-dependent state.
  explicit Saturation(double /*sampleRate*/) {}

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (mix_ <= 0.0f) return;
    const float driveLin = std::pow(10.0f, driveDb_ / 20.0f);
    const float outputGainLin = std::pow(10.0f, outputGainDb_ / 20.0f);
    const float asymBias = asymmetry_ * kAsymmetryRange;

    for (int32_t c = 0; c < numChannels && c < kMaxDspChannels; c++) {
      float* buf = channels[c];
      float& dcX1 = dcX1_[c];
      float& dcY1 = dcY1_[c];
      for (int32_t i = 0; i < numFrames; i++) {
        const float dry = buf[i];
        const float shaped = std::tanh(dry * driveLin + asymBias);

        // One-pole DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1], state clamped to [-1, 1] —
        // see the class comment on why the clamp is needed.
        const float blocked = std::clamp(shaped - dcX1 + kDcBlockCoeff * dcY1, -1.0f, 1.0f);
        dcX1 = shaped;
        dcY1 = blocked;

        const float wet = blocked * outputGainLin;
        buf[i] = dry * (1.0f - mix_) + wet * mix_;
      }
    }
  }

  void setParam(int32_t param, float value) override {
    if (param == static_cast<int32_t>(NodeParam::SatDrive)) {
      driveDb_ = std::clamp(value, 0.0f, 40.0f);
    } else if (param == static_cast<int32_t>(NodeParam::SatAsymmetry)) {
      asymmetry_ = std::clamp(value, -1.0f, 1.0f);
    } else if (param == static_cast<int32_t>(NodeParam::SatOutputGain)) {
      outputGainDb_ = std::clamp(value, -24.0f, 24.0f);
    } else if (param == static_cast<int32_t>(NodeParam::SatMix)) {
      mix_ = std::clamp(value, 0.0f, 1.0f);
    }
  }

  void reset() override {
    for (int32_t c = 0; c < kMaxDspChannels; c++) {
      dcX1_[c] = 0.0f;
      dcY1_[c] = 0.0f;
    }
  }

 private:
  // How far the tanh's input is biased per unit of asymmetry_ (+-1). Large enough to make
  // the asymmetry audible (a visibly lopsided transfer curve) without pinning the shaper
  // fully to one side across the whole [-1, 1] input range at asymmetry_ == +-1.
  static constexpr float kAsymmetryRange = 1.5f;
  // Close to 1 so the blocker only removes true DC / very-low-frequency drift, not bass
  // content — same order of magnitude as typical DC-blocker designs (e.g. R=0.995 at
  // 44.1/48kHz corresponds to a corner well below 20Hz).
  static constexpr float kDcBlockCoeff = 0.995f;

  float driveDb_ = 0.0f;        // NodeParam::SatDrive — pre-gain into the shaper
  float asymmetry_ = 0.0f;      // NodeParam::SatAsymmetry — -1..1, 0 = symmetric
  float outputGainDb_ = 0.0f;   // NodeParam::SatOutputGain — post-shaper makeup/trim
  float mix_ = 0.0f;            // NodeParam::SatMix — 0 => bypass, matches Delay/Reverb's convention

  float dcX1_[kMaxDspChannels] = {};
  float dcY1_[kMaxDspChannels] = {};
};

}  // namespace webdsp
