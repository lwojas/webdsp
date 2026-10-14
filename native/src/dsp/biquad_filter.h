#pragma once
#include <algorithm>
#include <cmath>
#include "../dsp_node.h"
#include "../params.h"

// Standard RBJ ("Audio EQ Cookbook") biquad lowpass/highpass. Investigated and preferred
// over pulling in a filter library: this is a ~40-line, well-documented, zero-dependency
// technique, appropriate for the "small, understandable core" the project favors over a
// large DSP dependency for something this size. See ARCHITECTURE.md, "DSP library
// investigation".
//
// Slope: 12 dB/oct (default) is a single biquad. 24 dB/oct cascades two identical-cutoff biquads
// with the classic 4th-order Butterworth Q split (0.5412 / 1.3066) rather than running the same
// biquad twice. Changing slope on a running filter crossfades between the two paths over
// kFadeFrames samples (the responses differ in phase, so a hard swap would step). See
// docs/filter-slope.md.
namespace webdsp {

constexpr int32_t kMaxDspChannels = 2;

class BiquadFilter final : public DSPNode {
 public:
  explicit BiquadFilter(double sampleRate) : sampleRate_(sampleRate) { recompute(); }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (bypassed_) return;
    const int32_t target = slope_ == FilterSlope::Db24 ? kFadeFrames : 0;
    const int32_t startPos = fadePos_;
    for (int32_t c = 0; c < numChannels && c < kMaxDspChannels; c++) {
      float* buf = channels[c];
      State& st = st_[c];
      int32_t pos = startPos;
      int32_t i = 0;

      // Crossfading between the 12 dB and 24 dB paths (only for a few ms after a slope change).
      for (; i < numFrames && pos != target; i++) {
        const double in = buf[i];
        if (st.stale12) { prime(c12_, in, st.a1, st.a2); st.stale12 = false; }
        if (st.stale24) {
          const double mid = prime(c24a_, in, st.b1, st.b2);
          prime(c24b_, mid, st.c1, st.c2);
          st.stale24 = false;
        }
        const double y12 = tick(c12_, st.a1, st.a2, in);
        const double y24 = tick(c24b_, st.c1, st.c2, tick(c24a_, st.b1, st.b2, in));
        pos += pos < target ? 1 : -1;
        const double w = static_cast<double>(pos) / kFadeFrames;
        buf[i] = static_cast<float>(y12 + (y24 - y12) * w);
      }

      if (target == 0) {
        for (; i < numFrames; i++) buf[i] = static_cast<float>(tick(c12_, st.a1, st.a2, buf[i]));
      } else {
        for (; i < numFrames; i++) {
          buf[i] = static_cast<float>(tick(c24b_, st.c1, st.c2, tick(c24a_, st.b1, st.b2, buf[i])));
        }
      }
    }
    hasProcessed_ = true;
    if (fadePos_ < target) fadePos_ = std::min(target, fadePos_ + numFrames);
    else if (fadePos_ > target) fadePos_ = std::max(target, fadePos_ - numFrames);
  }

  void setParam(int32_t param, float value) override {
    if (param == static_cast<int32_t>(VoiceParam::FilterCutoff)) {
      cutoffHz_ = value;
      bypassed_ = false;
      recompute();
    } else if (param == static_cast<int32_t>(VoiceParam::FilterResonance)) {
      q_ = value <= 0.0f ? 0.001f : value;
      recompute();
    } else if (param == static_cast<int32_t>(VoiceParam::FilterMode)) {
      mode_ = value >= 0.5f ? FilterMode::HighPass : FilterMode::LowPass;
      bypassed_ = false;
      recompute();
    } else if (param == static_cast<int32_t>(VoiceParam::FilterSlope)) {
      // Value is the slope in dB/oct. Anything nearer 24 than 12 selects 24; the filter stays
      // bypassed until a cutoff/mode is set, same as before.
      const FilterSlope next = value >= 18.0f ? FilterSlope::Db24 : FilterSlope::Db12;
      if (next == slope_) return;
      slope_ = next;
      if (!hasProcessed_) {
        // Nothing has run yet (fresh/reset filter, e.g. a voice being configured before its
        // first block): adopt the slope outright rather than fading in from the default.
        fadePos_ = slope_ == FilterSlope::Db24 ? kFadeFrames : 0;
      } else {
        // The path being faded in has been idle since the last fade finished; its state is
        // stale. If we're mid-fade both paths are still running and stay valid.
        for (State& st : st_) {
          if (slope_ == FilterSlope::Db24 && fadePos_ == 0) st.stale24 = true;
          if (slope_ == FilterSlope::Db12 && fadePos_ == kFadeFrames) st.stale12 = true;
        }
      }
    }
  }

  void reset() override {
    for (State& st : st_) st = State{};
    hasProcessed_ = false;
    fadePos_ = slope_ == FilterSlope::Db24 ? kFadeFrames : 0;
  }

 private:
  static constexpr int32_t kFadeFrames = 256;  // ~5 ms at 48 kHz

  struct Coeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  };

  // Per-channel state: a* = 12 dB biquad, b*/c* = the two 24 dB stages (transposed DF2).
  struct State {
    double a1 = 0, a2 = 0, b1 = 0, b2 = 0, c1 = 0, c2 = 0;
    bool stale12 = false, stale24 = false;
  };

  static double tick(const Coeffs& k, double& z1, double& z2, double in) {
    const double out = k.b0 * in + z1;
    z1 = k.b1 * in + z2 - k.a1 * out;
    z2 = k.b2 * in - k.a2 * out;
    return out;
  }

  // Puts a biquad in its DC steady state for constant input `x` (so a path that has been idle
  // joins a running signal without a transient step). Returns the DC output.
  static double prime(const Coeffs& k, double x, double& z1, double& z2) {
    const double den = 1.0 + k.a1 + k.a2;
    const double y = den > 1e-12 ? x * (k.b0 + k.b1 + k.b2) / den : x;
    z2 = k.b2 * x - k.a2 * y;
    z1 = k.b1 * x + z2 - k.a1 * y;
    return y;
  }

  Coeffs design(double omega, double q) const {
    const double sinO = std::sin(omega);
    const double cosO = std::cos(omega);
    const double alpha = sinO / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Coeffs k;
    if (mode_ == FilterMode::HighPass) {
      k.b0 = ((1.0 + cosO) / 2.0) / a0;
      k.b1 = -(1.0 + cosO) / a0;
    } else {
      k.b0 = ((1.0 - cosO) / 2.0) / a0;
      k.b1 = (1.0 - cosO) / a0;
    }
    k.b2 = k.b0;
    k.a1 = (-2.0 * cosO) / a0;
    k.a2 = (1.0 - alpha) / a0;
    return k;
  }

  void recompute() {
    double freq = cutoffHz_;
    if (freq < 20.0) freq = 20.0;
    if (freq > sampleRate_ * 0.49) freq = sampleRate_ * 0.49;
    const double omega = 2.0 * M_PI * freq / sampleRate_;

    c12_ = design(omega, q_);
    // 4th-order Butterworth pole pairs have Q 0.5412 and 1.3066 (product = 1/sqrt(2)).
    // `q_` is the user's resonance with 0.7071 meaning "flat Butterworth": it scales the
    // high-Q stage only, so the gain at cutoff is still q_ (0.5412 * 1.3066 * q_/0.7071) —
    // the same as the 12 dB mode — and q_ = 0.7071 is exactly maximally-flat 24 dB.
    constexpr double kQ1 = 0.54119610;
    constexpr double kQ2 = 1.30656296;
    constexpr double kButterQ = 0.70710678;
    c24a_ = design(omega, kQ1);
    c24b_ = design(omega, kQ2 * (static_cast<double>(q_) / kButterQ));
  }

  double sampleRate_;
  float cutoffHz_ = 20000.0f;
  float q_ = 0.707f;
  FilterMode mode_ = FilterMode::LowPass;
  FilterSlope slope_ = FilterSlope::Db12;
  bool bypassed_ = true;  // no cutoff/mode set yet => pass-through, avoids unwanted coloration
  bool hasProcessed_ = false;
  int32_t fadePos_ = 0;  // 0 = pure 12 dB path, kFadeFrames = pure 24 dB path

  Coeffs c12_, c24a_, c24b_;
  State st_[kMaxDspChannels];
};

}  // namespace webdsp
