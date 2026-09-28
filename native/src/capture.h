#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

// Minimal resample/capture proof of concept (ARCHITECTURE.md, "Resampling"): tap a bus's
// post-DSP output into a buffer, then hand the recorded audio back as plain PCM so the
// caller (engine.cpp) can register it as a brand new Sample. Deliberately not a general
// multi-track recorder — one capture at a time per captureId, append-only, no punch-in.
//
// Capacity is reserved up front (a non-realtime call, made from the worklet's message
// handler) so the realtime appendBlock() path never reallocates: it writes up to the
// reserved capacity and silently stops accepting frames beyond that, rather than growing on
// the audio thread. kMaxCaptureSeconds is a deliberate v1 limit on this proof of concept, not
// a permanent constraint on the runtime — see ARCHITECTURE.md, "Intentionally deferred", for
// a streaming/IndexedDB-backed capture path that would remove it.
//
// Two modes, both driven through the same appendBlock():
//  - Manual (start()/stop()): active immediately, runs until an explicit stop() — the
//    original proof-of-concept behavior, used by the demo app's CapturePanel.
//  - Armed (arm()): reserves exactly [startFrame, stopFrame) up front and only actually
//    copies frames once appendBlock() sees a render block overlapping that window, finishing
//    itself (isFinished()) the instant stopFrame is reached — sample-accurate start/stop
//    against the render clock, the same sub-block-delay idea Scheduler/Voice already use for
//    a scheduled trigger's delayFrames. This is what a caller needs to align a capture to an
//    application-level boundary (e.g. a sequencer's pattern loop) without any wall-clock
//    timing anywhere in the picture.
namespace webdsp {

constexpr double kMaxCaptureSeconds = 30.0;

class Capture {
 public:
  void start(int32_t channels, double sampleRate) {
    channels_ = channels;
    capacity_ = static_cast<int32_t>(sampleRate * kMaxCaptureSeconds);
    startFrame_ = 0;
    stopFrame_ = -1;  // unbounded — manual mode only ends via stop()
    length_ = 0;
    buffers_.assign(channels, std::vector<float>(capacity_, 0.0f));
    active_ = true;
    finished_ = false;
  }

  // Reserves capacity for exactly [startFrame, stopFrame) and arms the capture. Refuses (and
  // leaves the capture inactive) if the window is empty or exceeds kMaxCaptureSeconds worth
  // of frames at `sampleRate` — never silently truncates; a caller is expected to have
  // already checked this ahead of time (see RuntimeCapabilities.maxCaptureSeconds on the JS
  // side), so this is a defensive backstop that should be unreachable in normal use.
  bool arm(int32_t channels, double sampleRate, int64_t startFrame, int64_t stopFrame) {
    const int64_t frames = stopFrame - startFrame;
    const int64_t maxFrames = static_cast<int64_t>(sampleRate * kMaxCaptureSeconds);
    if (frames <= 0 || frames > maxFrames) {
      active_ = false;
      finished_ = false;
      return false;
    }
    channels_ = channels;
    startFrame_ = startFrame;
    stopFrame_ = stopFrame;
    capacity_ = static_cast<int32_t>(frames);
    length_ = 0;
    buffers_.assign(channels, std::vector<float>(capacity_, 0.0f));
    active_ = true;
    finished_ = false;
    return true;
  }

  // Realtime-safe: bounded by pre-reserved capacity, never allocates. `blockStartFrame` is
  // this render block's absolute frame position (same clock as ScheduledTrigger::atFrame) —
  // frames outside [startFrame_, stopFrame_) are skipped, and a boundary landing mid-block is
  // still honored on the exact sample via srcOffset, so an armed capture starts and stops
  // precisely regardless of render-quantum boundaries.
  void appendBlock(float* const* channels, int32_t numChannels, int32_t numFrames, int64_t blockStartFrame) {
    if (!active_) return;
    const int64_t blockEndFrame = blockStartFrame + numFrames;
    const int64_t windowEnd = stopFrame_ < 0 ? blockEndFrame : stopFrame_;  // -1 == unbounded
    if (blockEndFrame <= startFrame_ || blockStartFrame >= windowEnd) return;

    const int64_t copyStart = std::max(blockStartFrame, startFrame_);
    const int64_t copyEnd = std::min(blockEndFrame, windowEnd);
    const int32_t srcOffset = static_cast<int32_t>(copyStart - blockStartFrame);
    const int32_t room = capacity_ - length_;
    const int32_t toCopy = std::max(0, std::min(static_cast<int32_t>(copyEnd - copyStart), room));
    for (int32_t c = 0; c < channels_ && c < numChannels; c++) {
      std::copy(channels[c] + srcOffset, channels[c] + srcOffset + toCopy, buffers_[c].begin() + length_);
    }
    length_ += toCopy;

    if (stopFrame_ >= 0 && copyEnd >= stopFrame_) {
      active_ = false;
      finished_ = true;
    }
  }

  void stop() { active_ = false; }  // manual mode; a no-op once an armed capture has finished
  bool isActive() const { return active_; }
  // Only ever true for an arm()'d capture that reached its stopFrame — a manual start()/
  // stop() capture never sets this, since it has no scheduled end to detect.
  bool isFinished() const { return finished_; }
  int32_t channels() const { return channels_; }
  int32_t length() const { return length_; }
  const float* channelData(int32_t c) const { return buffers_[c].data(); }

 private:
  bool active_ = false;
  bool finished_ = false;
  int32_t channels_ = 0;
  int32_t capacity_ = 0;
  int32_t length_ = 0;
  int64_t startFrame_ = 0;
  int64_t stopFrame_ = -1;
  std::vector<std::vector<float>> buffers_;
};

}  // namespace webdsp
