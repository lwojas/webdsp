#pragma once
#include <algorithm>
#include <vector>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Simple feedback delay line, one instance per bus. A standard, small, well-understood
// technique — no external library warranted for this. Demonstrates a second, differently-
// shaped DSPNode (stateful circular buffer vs. the filter's IIR state) sharing the same
// interface, which is the point: the engine has no "effects subsystem", only DSPNode.
namespace webdsp {

class Delay final : public DSPNode {
 public:
  explicit Delay(double sampleRate, double maxSeconds = 2.0) : sampleRate_(sampleRate) {
    const int32_t capacity = static_cast<int32_t>(sampleRate * maxSeconds) + 1;
    for (int32_t c = 0; c < kMaxDspChannels; c++) {
      buffer_[c].assign(capacity, 0.0f);
    }
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (mix_ <= 0.0f) return;
    const int32_t delayFrames =
        std::clamp(static_cast<int32_t>(delaySeconds_ * sampleRate_), 1,
                    static_cast<int32_t>(buffer_[0].size()) - 1);

    for (int32_t c = 0; c < numChannels && c < kMaxDspChannels; c++) {
      float* buf = channels[c];
      auto& ring = buffer_[c];
      const int32_t capacity = static_cast<int32_t>(ring.size());
      int32_t w = writePos_[c];
      for (int32_t i = 0; i < numFrames; i++) {
        int32_t r = w - delayFrames;
        if (r < 0) r += capacity;
        const float wet = ring[r];
        const float in = buf[i];
        ring[w] = in + wet * feedback_;
        buf[i] = in * (1.0f - mix_) + wet * mix_;
        w = (w + 1) % capacity;
      }
      writePos_[c] = w;
    }
  }

  void setParam(int32_t param, float value) override {
    if (param == static_cast<int32_t>(NodeParam::DelayTime)) {
      delaySeconds_ = std::max(0.001f, value);
    } else if (param == static_cast<int32_t>(NodeParam::DelayFeedback)) {
      feedback_ = std::clamp(value, 0.0f, 0.98f);
    } else if (param == static_cast<int32_t>(NodeParam::DelayMix)) {
      mix_ = std::clamp(value, 0.0f, 1.0f);
    }
  }

  void reset() override {
    for (int32_t c = 0; c < kMaxDspChannels; c++) {
      std::fill(buffer_[c].begin(), buffer_[c].end(), 0.0f);
      writePos_[c] = 0;
    }
  }

 private:
  double sampleRate_;
  float delaySeconds_ = 0.25f;
  float feedback_ = 0.3f;
  float mix_ = 0.0f;  // 0 => bypass, matches Filter's "unset == pass-through" convention

  std::vector<float> buffer_[kMaxDspChannels];
  int32_t writePos_[kMaxDspChannels] = {};
};

}  // namespace webdsp
