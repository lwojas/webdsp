#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Chorus and flanger are the same primitive — one sine-LFO-modulated fractional-delay tap
// summed with dry, per docs/effects-algorithm-survey.md's ("Chorus/flanger") own framing and
// Dattorro's "Effect Design, Part 2: Delay-Line Modulation and Chorus" (JAES 45(10), 1997) —
// not two different algorithms needing two classes. This is one `DSPNode` whose parameter
// *ranges* pick which of the two a given setting sounds like: a longer center delay (~15-30ms)
// with little/no feedback is a chorus; a short center delay (~1-10ms) with feedback dialed up
// is a flanger's comb-filtered sweep. Same relationship as `Compressor`'s single `CompRatio`
// dial covering both "gentle bus glue" and "near-limiting" without two classes.
//
// Topology: dry sample -> write into a per-channel ring buffer (with feedback from the
// previous read folded back in, comb-filter style, same as `Delay`) -> read back at a
// sine-LFO-modulated fractional position -> linear-interpolated, wrapping read (the same
// technique `dsp/reverb.h`'s `ReverbModulatedAllpass::readWrapped` uses for its own modulated
// tap, and for the same reason `resampler.h`'s `linear()`/`cubicHermite()` aren't reused
// directly: those clamp at buffer edges for a single non-wrapping sample buffer, not wrap for
// a circular delay line) -> dry/wet mix. `ChorusFlangerStereoPhase` offsets channel 1's LFO
// phase from channel 0's (a fraction of one LFO cycle) — the standard way to widen a chorus in
// stereo without two independent, uncorrelated LFOs.
namespace webdsp {

class ChorusFlanger final : public DSPNode {
 public:
  explicit ChorusFlanger(double sampleRate) : sampleRate_(sampleRate) {
    // Capacity covers the worst case (max center delay + max depth, both in one direction)
    // plus a small margin for the linear interpolator's second tap.
    const int32_t capacity =
        static_cast<int32_t>(sampleRate * (kMaxDelayMs + kMaxDepthMs) / 1000.0) + 4;
    for (int32_t c = 0; c < kMaxDspChannels; c++) {
      buffer_[c].assign(static_cast<size_t>(capacity), 0.0f);
    }
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (mix_ <= 0.0f) return;
    const float delayFrames = static_cast<float>(delayMs_ * sampleRate_ / 1000.0);
    const float depthFrames = static_cast<float>(depthMs_ * sampleRate_ / 1000.0);
    const float lfoIncrement = static_cast<float>(2.0 * M_PI * rateHz_ / sampleRate_);
    const float stereoPhaseOffset = static_cast<float>(stereoPhase_ * 2.0 * M_PI);

    for (int32_t c = 0; c < numChannels && c < kMaxDspChannels; c++) {
      float* buf = channels[c];
      auto& ring = buffer_[c];
      const int32_t capacity = static_cast<int32_t>(ring.size());
      int32_t w = writePos_[c];
      const float channelPhaseOffset = c == 0 ? 0.0f : stereoPhaseOffset;

      for (int32_t i = 0; i < numFrames; i++) {
        const float lfo = std::sin(phase_ + channelPhaseOffset + lfoIncrement * i);
        // Clamped to a small positive floor: a total delay at or below zero would mean
        // reading a sample not written yet (not a causal delay line at all), reachable
        // whenever depth is set larger than the center delay.
        const float totalDelay =
            std::max(kMinDelayFrames, delayFrames + depthFrames * lfo);
        const double readPos = static_cast<double>(w) - totalDelay;
        const float wet = readWrapped(ring, readPos);

        const float in = buf[i];
        ring[w] = in + wet * feedback_;
        buf[i] = in * (1.0f - mix_) + wet * mix_;
        w = (w + 1) % capacity;
      }
      writePos_[c] = w;
    }

    phase_ += lfoIncrement * numFrames;
    phase_ = std::fmod(phase_, static_cast<float>(2.0 * M_PI));
  }

  void setParam(int32_t param, float value) override {
    if (param == static_cast<int32_t>(NodeParam::ChorusFlangerRate)) {
      rateHz_ = std::clamp(value, 0.01f, 10.0f);
    } else if (param == static_cast<int32_t>(NodeParam::ChorusFlangerDepth)) {
      depthMs_ = std::clamp(value, 0.0f, kMaxDepthMs);
    } else if (param == static_cast<int32_t>(NodeParam::ChorusFlangerDelay)) {
      delayMs_ = std::clamp(value, 0.1f, kMaxDelayMs);
    } else if (param == static_cast<int32_t>(NodeParam::ChorusFlangerFeedback)) {
      feedback_ = std::clamp(value, -0.95f, 0.95f);
    } else if (param == static_cast<int32_t>(NodeParam::ChorusFlangerStereoPhase)) {
      stereoPhase_ = std::clamp(value, 0.0f, 1.0f);
    } else if (param == static_cast<int32_t>(NodeParam::ChorusFlangerMix)) {
      mix_ = std::clamp(value, 0.0f, 1.0f);
    }
  }

  void reset() override {
    for (int32_t c = 0; c < kMaxDspChannels; c++) {
      std::fill(buffer_[c].begin(), buffer_[c].end(), 0.0f);
      writePos_[c] = 0;
    }
    phase_ = 0.0f;
  }

 private:
  // Same wrapping linear interpolation as `dsp/reverb.h`'s `ReverbModulatedAllpass::
  // readWrapped` — see the class comment for why a plain (non-wrapping) interpolator from
  // resampler.h isn't reused here.
  static float readWrapped(const std::vector<float>& ring, double pos) {
    const int32_t n = static_cast<int32_t>(ring.size());
    int32_t i0 = static_cast<int32_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(i0);
    int32_t idx0 = i0 % n;
    if (idx0 < 0) idx0 += n;
    const int32_t idx1 = (idx0 + 1) % n;
    return static_cast<float>(ring[static_cast<size_t>(idx0)] * (1.0 - frac) +
                               ring[static_cast<size_t>(idx1)] * frac);
  }

  // Chorus needs up to ~30ms of center delay; flanger's feel comes from short delay + depth
  // interacting, not from depth alone, so both share this one range rather than two separate
  // per-mode limits.
  static constexpr float kMaxDelayMs = 40.0f;
  static constexpr float kMaxDepthMs = 20.0f;
  static constexpr float kMinDelayFrames = 1.0f;

  double sampleRate_;
  float rateHz_ = 0.5f;        // NodeParam::ChorusFlangerRate — LFO speed
  float depthMs_ = 3.0f;       // NodeParam::ChorusFlangerDepth — LFO peak excursion
  float delayMs_ = 15.0f;      // NodeParam::ChorusFlangerDelay — center/base delay
  float feedback_ = 0.0f;      // NodeParam::ChorusFlangerFeedback — comb resonance; negative
                                // for flanger's "through-zero"-flavored notch character
  float stereoPhase_ = 0.25f;  // NodeParam::ChorusFlangerStereoPhase — fraction of one LFO
                                // cycle channel 1's LFO leads channel 0's by
  float mix_ = 0.0f;           // NodeParam::ChorusFlangerMix — 0 => bypass, matches
                                // Delay/Reverb/Saturation's convention

  float phase_ = 0.0f;  // shared LFO phase accumulator, channel offset applied at read time
  std::vector<float> buffer_[kMaxDspChannels];
  int32_t writePos_[kMaxDspChannels] = {};
};

}  // namespace webdsp
