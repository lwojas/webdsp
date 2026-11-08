#pragma once
#include <algorithm>
#include <cmath>
#include "../dsp_node.h"
#include "../params.h"
#include "character_filter.h"
#include "filter_modulation.h"

// The Character Filter as an effect: a stereo pair of CharacterFilter (dsp/character_filter.h, which
// holds the design) driven by NodeParams, for any bus: a drum track, a sampler, a synth, the Noise
// instrument. Synths that want it inside a voice use CharacterFilter directly and keep their own
// envelope and keyboard tracking.
//
// Env: an effect has no note and no envelope, so the modulation source here is the signal itself,
// an envelope follower on the input (about 2 ms attack, 140 ms release). EnvAmount (-1..1) moves the
// cutoff up (positive) or down (negative) by up to kFilterModMaxOctaves octaves in proportion to the
// input level, through FilterModulation like every other synth does. Zero (the default) leaves the
// cutoff exactly where Cutoff puts it. There is no keyboard tracking here (no pitch); a synth passes
// CharacterFilter::trackedCutoff() as its cutoff.
//
// All parameters are safe to move at any rate (the filter smooths them); non-finite values are
// ignored. With the defaults (cutoff 18 kHz, resonance 0, drive 0) it is a light, warm colouring, not
// a bypass; remove or disable the instance to bypass it.
namespace webdsp {

class CharacterFilterFx final : public DSPNode {
 public:
  explicit CharacterFilterFx(double sampleRate) {
    mod_.configure(sampleRate);
    for (CharacterFilter& f : filters_) {
      f.configure(sampleRate);
      f.setCharacter(character_);
    }
    attack_ = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * 0.002));
    release_ = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * 0.140));
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    const int32_t nc = std::min(numChannels, kMaxDspChannels);
    for (int32_t pos = 0; pos < numFrames; pos += kStep) {
      const int32_t m = std::min(kStep, numFrames - pos);
      float cutoff = cutoffHz_;
      if (envAmount_ != 0.0f) {
        for (int32_t i = 0; i < m; i++) {
          float level = 0.0f;
          for (int32_t c = 0; c < nc; c++) level = std::max(level, std::fabs(channels[c][pos + i]));
          if (!std::isfinite(level)) level = 0.0f;
          env_ += (level > env_ ? attack_ : release_) * (level - env_);
        }
        const float e = 1.0f - std::exp(-5.0f * env_);
        cutoff = mod_.cutoffHz(cutoffHz_, kFilterModMaxOctaves, envAmount_ * e);
      }
      for (int32_t c = 0; c < nc; c++) filters_[c].process(channels[c] + pos, m, cutoff);
    }
  }

  void setParam(int32_t param, float value) override {
    if (!std::isfinite(value)) return;
    switch (static_cast<NodeParam>(param)) {
      case NodeParam::CharFilterMode: {
        mode_ = std::clamp(static_cast<int32_t>(std::lround(value)), 0, CharacterFilter::kModes - 1);
        for (CharacterFilter& f : filters_) f.setMode(mode_);
        break;
      }
      case NodeParam::CharFilterCutoff:
        cutoffHz_ = std::clamp(value, FilterModulation::kMinHz, mod_.maxHz());
        break;
      case NodeParam::CharFilterResonance:
        for (CharacterFilter& f : filters_) f.setResonance(value);
        break;
      case NodeParam::CharFilterDrive:
        for (CharacterFilter& f : filters_) f.setDrive(value);
        break;
      case NodeParam::CharFilterCharacter:
        character_ = std::clamp(value, 0.0f, 1.0f);
        for (CharacterFilter& f : filters_) f.setCharacter(character_);
        break;
      case NodeParam::CharFilterEnvAmount:
        envAmount_ = std::clamp(value, -1.0f, 1.0f);
        break;
      default: break;
    }
  }

  void reset() override {
    for (CharacterFilter& f : filters_) f.reset();
    env_ = 0.0f;
  }

 private:
  static constexpr int32_t kStep = 16;  // control step: envelope read-out and cutoff update (~0.33 ms)

  FilterModulation mod_;
  CharacterFilter filters_[kMaxDspChannels];
  int32_t mode_ = 0;
  float cutoffHz_ = 18000.0f;
  float character_ = 0.4f;
  float envAmount_ = 0.0f;
  float env_ = 0.0f;
  float attack_ = 0.0f, release_ = 0.0f;
};

}  // namespace webdsp
