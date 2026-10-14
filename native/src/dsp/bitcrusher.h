#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Bitcrusher: amplitude quantization to `bits` plus sample-and-hold decimation by `downsample`
// (hold each input sample for N frames), blended with the dry signal. Deliberately aliasing —
// no anti-alias filtering — since that is the effect. Sample-rate independent: the hold is a
// frame count, not Hz.
namespace webdsp {

class Bitcrusher final : public DSPNode {
 public:
  explicit Bitcrusher(double /*sampleRate*/) {}

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (mix_ <= 0.0f) return;
    const float steps = std::exp2(bits_ - 1.0f);  // quantization steps per unit amplitude
    for (int32_t c = 0; c < numChannels && c < kMaxDspChannels; c++) {
      float* buf = channels[c];
      int32_t counter = counter_[c];
      float held = held_[c];
      for (int32_t i = 0; i < numFrames; i++) {
        const float dry = buf[i];
        if (counter <= 0) {
          // Exactly 2^bits levels: codes -steps .. steps-1 (two's-complement style).
          held = std::clamp(std::round(dry * steps), -steps, steps - 1.0f) / steps;
          counter = downsample_;
        }
        counter--;
        buf[i] = dry * (1.0f - mix_) + held * mix_;
      }
      counter_[c] = counter;
      held_[c] = held;
    }
  }

  void setParam(int32_t param, float value) override {
    switch (static_cast<NodeParam>(param)) {
      case NodeParam::CrushBits: bits_ = std::clamp(value, 1.0f, 16.0f); break;
      case NodeParam::CrushDownsample:
        downsample_ = std::clamp(static_cast<int32_t>(std::lround(value)), 1, 64);
        break;
      case NodeParam::CrushMix: mix_ = std::clamp(value, 0.0f, 1.0f); break;
      default: break;
    }
  }

  void reset() override {
    for (int32_t c = 0; c < kMaxDspChannels; c++) {
      counter_[c] = 0;
      held_[c] = 0.0f;
    }
  }

 private:
  float bits_ = 16.0f;
  int32_t downsample_ = 1;
  float mix_ = 0.0f;
  int32_t counter_[kMaxDspChannels] = {};
  float held_[kMaxDspChannels] = {};
};

}  // namespace webdsp
