#pragma once
#include <cmath>
#include "../dsp_node.h"
#include "../params.h"

// Standard RBJ ("Audio EQ Cookbook") biquad lowpass/highpass. Investigated and preferred
// over pulling in a filter library: this is a ~40-line, well-documented, zero-dependency
// technique, appropriate for the "small, understandable core" the project favors over a
// large DSP dependency for something this size. See ARCHITECTURE.md, "DSP library
// investigation".
namespace webdsp {

constexpr int32_t kMaxDspChannels = 2;

class BiquadFilter final : public DSPNode {
 public:
  explicit BiquadFilter(double sampleRate) : sampleRate_(sampleRate) { recompute(); }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (bypassed_) return;
    for (int32_t c = 0; c < numChannels && c < kMaxDspChannels; c++) {
      float* buf = channels[c];
      double z1 = z1_[c];
      double z2 = z2_[c];
      for (int32_t i = 0; i < numFrames; i++) {
        double in = buf[i];
        double out = b0_ * in + z1;
        z1 = b1_ * in + z2 - a1_ * out;
        z2 = b2_ * in - a2_ * out;
        buf[i] = static_cast<float>(out);
      }
      z1_[c] = z1;
      z2_[c] = z2;
    }
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
    }
  }

  void reset() override {
    for (int32_t c = 0; c < kMaxDspChannels; c++) z1_[c] = z2_[c] = 0.0;
  }

 private:
  void recompute() {
    double freq = cutoffHz_;
    if (freq < 20.0) freq = 20.0;
    if (freq > sampleRate_ * 0.49) freq = sampleRate_ * 0.49;

    const double omega = 2.0 * M_PI * freq / sampleRate_;
    const double sinO = std::sin(omega);
    const double cosO = std::cos(omega);
    const double alpha = sinO / (2.0 * q_);

    const double a0 = 1.0 + alpha;
    if (mode_ == FilterMode::HighPass) {
      b0_ = ((1.0 + cosO) / 2.0) / a0;
      b1_ = -(1.0 + cosO) / a0;
      b2_ = b0_;
    } else {
      b0_ = ((1.0 - cosO) / 2.0) / a0;
      b1_ = (1.0 - cosO) / a0;
      b2_ = b0_;
    }
    a1_ = (-2.0 * cosO) / a0;
    a2_ = (1.0 - alpha) / a0;
  }

  double sampleRate_;
  float cutoffHz_ = 20000.0f;
  float q_ = 0.707f;
  FilterMode mode_ = FilterMode::LowPass;
  bool bypassed_ = true;  // no cutoff/mode set yet => pass-through, avoids unwanted coloration

  double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
  double z1_[kMaxDspChannels] = {};
  double z2_[kMaxDspChannels] = {};
};

}  // namespace webdsp
