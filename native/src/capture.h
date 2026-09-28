#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

// Minimal resample/capture proof of concept (ARCHITECTURE.md, "Resampling"): tap a bus's
// post-DSP output into a buffer, then hand the recorded audio back as plain PCM so the
// caller (engine.cpp) can register it as a brand new Sample. Deliberately not a general
// multi-track recorder — one capture at a time per captureId, append-only, no punch-in.
//
// Capacity is reserved up front in start() (a non-realtime call, made from the worklet's
// message handler) so the realtime appendBlock() path never reallocates: it writes up to
// the reserved capacity and silently stops accepting frames beyond that, rather than
// growing on the audio thread. kMaxCaptureSeconds is a deliberate v1 limit on this proof of
// concept, not a permanent constraint on the runtime — see ARCHITECTURE.md, "Intentionally
// deferred", for a streaming/IndexedDB-backed capture path that would remove it.
namespace webdsp {

constexpr double kMaxCaptureSeconds = 30.0;

class Capture {
 public:
  void start(int32_t channels, double sampleRate) {
    channels_ = channels;
    capacity_ = static_cast<int32_t>(sampleRate * kMaxCaptureSeconds);
    length_ = 0;
    buffers_.assign(channels, std::vector<float>(capacity_, 0.0f));
    active_ = true;
  }

  // Realtime-safe: bounded by pre-reserved capacity, never allocates.
  void appendBlock(float* const* channels, int32_t numChannels, int32_t numFrames) {
    if (!active_) return;
    const int32_t room = capacity_ - length_;
    const int32_t toCopy = std::max(0, std::min(numFrames, room));
    for (int32_t c = 0; c < channels_ && c < numChannels; c++) {
      std::copy(channels[c], channels[c] + toCopy, buffers_[c].begin() + length_);
    }
    length_ += toCopy;
  }

  void stop() { active_ = false; }
  bool isActive() const { return active_; }
  int32_t channels() const { return channels_; }
  int32_t length() const { return length_; }
  const float* channelData(int32_t c) const { return buffers_[c].data(); }

 private:
  bool active_ = false;
  int32_t channels_ = 0;
  int32_t capacity_ = 0;
  int32_t length_ = 0;
  std::vector<std::vector<float>> buffers_;
};

}  // namespace webdsp
