#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "biquad_filter.h"  // kMaxDspChannels

// Master-bus peak limiter: the final stage of the signal path (Engine::process, after the master
// bus chain). Not a DSPNode and not part of any FX chain: it exists once, on the master output.
//
// Zero-latency design. Gain is computed per sample from the stereo-linked peak so that
// |x| * gain <= ceiling holds for every sample by construction (instant attack), then recovers
// towards unity with a one-pole release. No look-ahead means no added latency, so sample-accurate
// scheduling and captures are unaffected; the price is that a hard transient is turned down
// within the sample it occurs in rather than ahead of time, which is acceptable for a safety
// limiter whose job is to keep the final output from clipping, not to shape the mix. A signal
// at or below the ceiling gets exactly gain 1 (bit-transparent).
//
// Not allocation-free to construct (none needed), allocation-free to run. Deterministic.
namespace webdsp {

class Limiter {
 public:
  static constexpr float kDefaultCeilingDb = -0.3f;
  static constexpr float kDefaultReleaseMs = 100.0f;

  void configure(double sampleRate) {
    sampleRate_ = sampleRate;
    recomputeRelease();
    gain_ = 1.0f;
    minGain_ = 1.0f;
  }

  void setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_) gain_ = 1.0f;
  }
  void setCeilingDb(float db) {
    ceilingDb_ = std::clamp(db, -24.0f, 0.0f);
    ceiling_ = std::pow(10.0f, ceilingDb_ / 20.0f);
  }
  void setReleaseMs(float ms) {
    releaseMs_ = std::clamp(ms, 5.0f, 2000.0f);
    recomputeRelease();
  }
  bool enabled() const { return enabled_; }
  float ceilingDb() const { return ceilingDb_; }
  float releaseMs() const { return releaseMs_; }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) {
    const int32_t n = std::min(numChannels, kMaxDspChannels);
    if (!enabled_) return;
    for (int32_t i = 0; i < numFrames; i++) {
      float peak = 0.0f;
      for (int32_t c = 0; c < n; c++) peak = std::max(peak, std::fabs(channels[c][i]));
      // Recover towards unity, but never past what this sample allows (instant attack).
      float g = gain_ < 1.0f ? 1.0f - (1.0f - gain_) * releaseCoeff_ : 1.0f;
      if (g > 0.99999f) g = 1.0f;
      if (peak * g > ceiling_) g = ceiling_ / peak;
      gain_ = g;
      if (gain_ < 1.0f) {
        for (int32_t c = 0; c < n; c++) channels[c][i] *= gain_;
      }
      minGain_ = std::min(minGain_, gain_);
    }
  }

  // Deepest gain applied since the last call, in dB (<= 0); resets the tracker. This is the
  // metering view of "how hard did the limiter work".
  float takeGainReductionDb() {
    const float db = 20.0f * std::log10(std::max(minGain_, 1e-6f));
    minGain_ = gain_;
    return db;
  }

  void reset() {
    gain_ = 1.0f;
    minGain_ = 1.0f;
  }

 private:
  void recomputeRelease() {
    releaseCoeff_ = static_cast<float>(std::exp(-1.0 / (releaseMs_ * 0.001 * sampleRate_)));
  }

  double sampleRate_ = 48000.0;
  bool enabled_ = true;
  float ceilingDb_ = kDefaultCeilingDb;
  float ceiling_ = std::pow(10.0f, kDefaultCeilingDb / 20.0f);
  float releaseMs_ = kDefaultReleaseMs;
  float releaseCoeff_ = 0.9998f;
  float gain_ = 1.0f;
  float minGain_ = 1.0f;
};

}  // namespace webdsp
