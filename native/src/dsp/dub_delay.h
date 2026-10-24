#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Dub Delay: a tempo-synchronised feedback delay. The delay time is not a parameter in seconds but
// a musical division of the transport's beat (setTempo, fed by FxChain from the engine's beat
// grid), so it follows BPM changes by itself. The tone control is a one-pole low-pass INSIDE the
// feedback loop (delay out -> low-pass -> feedback gain -> delay in): each repeat is filtered once
// more than the one before, so echoes darken as they fade, the signature of a dub delay.
//
//   in ---+---------------------------> dry * (1-mix) + wet * mix --> out
//         |                      ^
//         +--> [ring buffer] ----+--> wet
//               ^                    |
//               +-- feedback * LP <--+
//
// Deliberately small: division, feedback, tone, mix. No modulation, saturation, ping-pong or
// diffusion. Stereo follows Delay: independent ring + filter state per channel, same time.
//
// Time changes (a new division, a BPM move) slew the read position at a bounded rate with linear
// interpolation instead of jumping it, so the echoes glide in pitch like a tape/BBD delay rather
// than clicking. Settled, the time is a whole number of frames, so the interpolation is exact and
// adds no extra filtering to the loop.
namespace webdsp {

// Musical divisions, in sixteenth notes. Index = the DubDelayDivision value.
inline constexpr int32_t kDubDelayDivisionCount = 6;
inline constexpr int32_t kDubDelaySixteenths[kDubDelayDivisionCount] = {
    1,  // 1/16
    2,  // 1/8
    3,  // 1/8 dotted
    4,  // 1/4
    6,  // 1/4 dotted
    8,  // 1/2
};

class DubDelay final : public DSPNode {
 public:
  // Longest delay the buffer holds. A division longer than this at a slow tempo is halved until it
  // fits (still on the beat grid, one or more octaves down in rate).
  static constexpr double kMaxSeconds = 4.0;

  explicit DubDelay(double sampleRate) : sampleRate_(sampleRate) {
    capacity_ = static_cast<int32_t>(sampleRate * kMaxSeconds) + 1;
    for (int32_t c = 0; c < kMaxDspChannels; c++) ring_[c].assign(capacity_, 0.0f);
    smooth_ = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * 0.01));
    setTone(3000.0f);
    updateTarget();
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    // Bypassed (mix 0, glide finished): drop the memory once so raising the mix later starts
    // clean instead of replaying audio from before the bypass (same rule as Delay).
    if (mixTarget_ <= 0.0f && mixCur_ <= kSilence) {
      if (dirty_) {
        reset();
        dirty_ = false;
      }
      return;
    }
    dirty_ = true;
    if (!started_) {  // nothing to glide from: start at the tempo-derived time
      delayCur_ = targetFrames_;
      fbCur_ = fbTarget_;
      started_ = true;
    }
    const int32_t nc = std::min(numChannels, kMaxDspChannels);
    const double target = targetFrames_;
    double delayEnd = delayCur_;
    float mixEnd = mixCur_, fbEnd = fbCur_;
    for (int32_t c = 0; c < nc; c++) {
      float* buf = channels[c];
      std::vector<float>& ring = ring_[c];
      int32_t w = writePos_[c];
      double d = delayCur_;  // every channel replays the same glide
      float mix = mixCur_, fb = fbCur_, lp = lp_[c];
      for (int32_t i = 0; i < numFrames; i++) {
        d += std::clamp(target - d, -kMaxSlew, kMaxSlew);
        mix += smooth_ * (mixTarget_ - mix);
        fb += smooth_ * (fbTarget_ - fb);

        double r = static_cast<double>(w) - d;
        if (r < 0.0) r += capacity_;
        const int32_t r0 = static_cast<int32_t>(r);
        const float frac = static_cast<float>(r - r0);
        const int32_t r1 = r0 + 1 == capacity_ ? 0 : r0 + 1;
        const float wet = ring[r0] + frac * (ring[r1] - ring[r0]);

        // The feedback path: low-pass the echo, scale it, write it back with the new input.
        lp = lpCoef_ * lp + (1.0f - lpCoef_) * wet;
        if (std::fabs(lp) < 1e-20f) lp = 0.0f;  // no denormals once a tail has died away
        const float in = buf[i];
        ring[w] = in + lp * fb;
        buf[i] = in * (1.0f - mix) + wet * mix;
        if (++w == capacity_) w = 0;
      }
      writePos_[c] = w;
      lp_[c] = lp;
      delayEnd = d;
      mixEnd = mix;
      fbEnd = fb;
    }
    delayCur_ = delayEnd;
    mixCur_ = std::fabs(mixTarget_ - mixEnd) < kSilence ? mixTarget_ : mixEnd;
    fbCur_ = fbEnd;
  }

  void setParam(int32_t param, float value) override {
    if (!std::isfinite(value)) return;
    switch (static_cast<NodeParam>(param)) {
      case NodeParam::DubDelayDivision:
        division_ = std::clamp(static_cast<int32_t>(std::lround(value)), 0, kDubDelayDivisionCount - 1);
        updateTarget();
        break;
      case NodeParam::DubDelayFeedback: fbTarget_ = std::clamp(value, 0.0f, 0.95f); break;
      case NodeParam::DubDelayTone: setTone(value); break;
      case NodeParam::DubDelayMix: mixTarget_ = std::clamp(value, 0.0f, 1.0f); break;
      default: break;
    }
  }

  void setTempo(double framesPerBeat) override {
    if (!(framesPerBeat > 0.0) || !std::isfinite(framesPerBeat)) return;
    framesPerBeat_ = framesPerBeat;
    updateTarget();
  }

  void reset() override {
    for (int32_t c = 0; c < kMaxDspChannels; c++) {
      std::fill(ring_[c].begin(), ring_[c].end(), 0.0f);
      writePos_[c] = 0;
      lp_[c] = 0.0f;
    }
    mixCur_ = 0.0f;  // the dry/wet balance glides in from the dry signal
    started_ = false;
  }

  // Exposed for tests: the settled delay in frames for the current division and tempo.
  double targetFrames() const { return targetFrames_; }

 private:
  static constexpr double kMaxSlew = 0.4;   // frames of read-position change per sample (pitch +-40%)
  static constexpr float kSilence = 1e-4f;  // below this the mix glide counts as finished

  void updateTarget() {
    double frames = framesPerBeat_ * 0.25 * kDubDelaySixteenths[division_];
    const double maxFrames = static_cast<double>(capacity_ - 3);
    while (frames > maxFrames) frames *= 0.5;
    targetFrames_ = std::max(2.0, std::round(frames));
  }

  void setTone(float hz) {
    const float f = std::clamp(hz, 200.0f, 16000.0f);
    lpCoef_ = std::exp(-6.2831853f * f / static_cast<float>(sampleRate_));
  }

  double sampleRate_;
  int32_t capacity_ = 0;
  float smooth_ = 0.002f;

  int32_t division_ = 2;  // 1/8 dotted
  double framesPerBeat_ = 24000.0;
  double targetFrames_ = 0.0;
  double delayCur_ = 0.0;
  float fbTarget_ = 0.45f, fbCur_ = 0.45f;
  float mixTarget_ = 0.0f, mixCur_ = 0.0f;  // 0 => bypass, like Delay
  float lpCoef_ = 0.0f;

  std::vector<float> ring_[kMaxDspChannels];
  int32_t writePos_[kMaxDspChannels] = {};
  float lp_[kMaxDspChannels] = {};
  bool started_ = false;
  bool dirty_ = false;  // processed since the last reset(): the buffers may hold audio
};

}  // namespace webdsp
