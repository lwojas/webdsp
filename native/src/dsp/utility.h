#pragma once
#include <algorithm>
#include <cmath>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Utility: a minimal gain stage, the DSP building block behind per-track volume automation.
// One parameter, gain in dB (kUtilityMinDb..kUtilityMaxDb); the bottom of the range is true
// silence. Unity (0 dB) is the neutral state, so a freshly added Utility passes audio untouched.
//
// The gain is smoothed (one-pole, ~5 ms) so a parameter driven by coarse automation steps or an
// LFO never zippers. Deliberately not an Ableton-style multi-function device: width, mono,
// phase and DC offset can be added later as further NodeParams without changing this contract.
namespace webdsp {

inline constexpr float kUtilityMinDb = -60.0f;
inline constexpr float kUtilityMaxDb = 12.0f;

class Utility final : public DSPNode {
 public:
  explicit Utility(double sampleRate) {
    coef_ = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * 0.005));
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (!started_) {
      current_ = target_;
      started_ = true;
    }
    if (current_ == target_ && target_ == 1.0f) return;  // unity: bit-exact pass-through
    const int32_t nc = std::min(numChannels, kMaxDspChannels);
    float g = current_;
    for (int32_t i = 0; i < numFrames; i++) {
      g += coef_ * (target_ - g);
      if (std::fabs(target_ - g) < 1e-6f) g = target_;
      for (int32_t c = 0; c < nc; c++) channels[c][i] *= g;
    }
    current_ = g;
  }

  void setParam(int32_t param, float value) override {
    if (param != static_cast<int32_t>(NodeParam::UtilityGain) || !std::isfinite(value)) return;
    const float db = std::clamp(value, kUtilityMinDb, kUtilityMaxDb);
    target_ = db <= kUtilityMinDb ? 0.0f : std::pow(10.0f, db / 20.0f);
  }

  void reset() override {
    started_ = false;  // re-enabling snaps to the target instead of gliding from a stale gain
  }

 private:
  float coef_ = 0.01f;
  float target_ = 1.0f;
  float current_ = 1.0f;
  bool started_ = false;
};

}  // namespace webdsp
