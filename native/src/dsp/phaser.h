#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Phaser: a chain of first-order all-pass stages whose break frequency is swept by a sine LFO,
// summed with the dry signal so the moving phase shifts show up as moving notches. Optional
// feedback around the stage chain deepens/resonates the notches.
//
// Topology per channel: x = dry + feedback * lastWet -> N all-pass stages -> wet; out =
// dry*(1-mix) + wet*mix. Stage coefficient a = (tan(pi f / fs) - 1) / (tan(pi f / fs) + 1)
// with f = center * 2^(2 * depth * lfo), so depth 1 sweeps +-2 octaves around `center`.
// Channel 1's LFO leads channel 0's by a fixed quarter cycle for stereo width.
// No DSP state is shared between instances; this class holds everything it needs.
namespace webdsp {

class Phaser final : public DSPNode {
 public:
  static constexpr int32_t kMaxStages = 12;

  explicit Phaser(double sampleRate) : sampleRate_(sampleRate) {}

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (mix_ <= 0.0f) return;
    const double lfoInc = rateHz_ / sampleRate_;
    const double nyquistCap = 0.45 * sampleRate_;
    const double pi = 3.14159265358979323846;

    for (int32_t c = 0; c < numChannels && c < kMaxDspChannels; c++) {
      float* buf = channels[c];
      double phase = lfoPhase_ + (c == 1 ? kStereoPhase : 0.0);
      float* z = z_[c];
      float fb = fb_[c];
      for (int32_t i = 0; i < numFrames; i++) {
        phase -= std::floor(phase);
        const double lfo = std::sin(2.0 * pi * phase);
        phase += lfoInc;
        const double f = std::clamp(centerHz_ * std::exp2(2.0 * depth_ * lfo), 20.0, nyquistCap);
        const double t = std::tan(pi * f / sampleRate_);
        const float a = static_cast<float>((t - 1.0) / (t + 1.0));

        const float dry = buf[i];
        float x = dry + feedback_ * fb;
        for (int32_t s = 0; s < stages_; s++) {
          const float y = a * x + z[s];
          z[s] = x - a * y;
          x = y;
        }
        fb = std::clamp(x, -4.0f, 4.0f);  // keeps a resonant feedback loop bounded
        buf[i] = dry * (1.0f - mix_) + x * mix_;
      }
      fb_[c] = fb;
    }
    lfoPhase_ += lfoInc * numFrames;
    lfoPhase_ -= std::floor(lfoPhase_);
  }

  void setParam(int32_t param, float value) override {
    switch (static_cast<NodeParam>(param)) {
      case NodeParam::PhaserRate: rateHz_ = std::clamp(value, 0.01f, 10.0f); break;
      case NodeParam::PhaserDepth: depth_ = std::clamp(value, 0.0f, 1.0f); break;
      case NodeParam::PhaserCenter: centerHz_ = std::clamp(value, 100.0f, 5000.0f); break;
      case NodeParam::PhaserFeedback: feedback_ = std::clamp(value, -0.95f, 0.95f); break;
      case NodeParam::PhaserStages: {
        // Even stage counts only (each notch needs a pair of all-pass stages).
        const int32_t n = static_cast<int32_t>(std::lround(value / 2.0f)) * 2;
        stages_ = std::clamp(n, 2, kMaxStages);
        break;
      }
      case NodeParam::PhaserMix: mix_ = std::clamp(value, 0.0f, 1.0f); break;
      default: break;
    }
  }

  void reset() override {
    for (int32_t c = 0; c < kMaxDspChannels; c++) {
      for (float& s : z_[c]) s = 0.0f;
      fb_[c] = 0.0f;
    }
    lfoPhase_ = 0.0;
  }

 private:
  static constexpr double kStereoPhase = 0.25;

  double sampleRate_;
  float rateHz_ = 0.5f;
  float depth_ = 0.5f;
  float centerHz_ = 800.0f;
  float feedback_ = 0.0f;
  int32_t stages_ = 4;
  float mix_ = 0.0f;  // 0 => bypass, same "inert until parameterized" convention as the others

  double lfoPhase_ = 0.0;
  float z_[kMaxDspChannels][kMaxStages] = {};
  float fb_[kMaxDspChannels] = {};
};

}  // namespace webdsp
