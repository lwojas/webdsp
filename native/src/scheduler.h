#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

// Sample-accurate event queue. This is the engine's entire "sequencing" surface: it knows
// nothing about patterns, steps, or tracks — only "trigger this at this absolute frame".
// The authoritative time base is the audio render clock (frames since the AudioContext
// started, from AudioWorkletGlobalScope.currentFrame on the JS side), never a JS timer —
// see ARCHITECTURE.md, "Scheduling".
namespace webdsp {

struct ScheduledTrigger {
  int64_t atFrame;
  int32_t voiceId;
  int32_t sampleId;
  int32_t busId;
  float gain;
  float rate;
  int32_t startFrame;
  int32_t endFrame;
  bool loop;
  bool reverse;
};

class Scheduler {
 public:
  void push(const ScheduledTrigger& event) {
    // Insertion-sorted by time; the queue is small (a UI/sequencer's lookahead window, not
    // an entire song) so this is simpler and cheap enough to be preferable to a heap here.
    auto it = std::upper_bound(queue_.begin(), queue_.end(), event,
                                [](const ScheduledTrigger& a, const ScheduledTrigger& b) {
                                  return a.atFrame < b.atFrame;
                                });
    queue_.insert(it, event);
  }

  void cancelFrom(int64_t fromFrame) {
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                 [&](const ScheduledTrigger& e) { return e.atFrame >= fromFrame; }),
                 queue_.end());
  }

  // Invokes `fn(event)` for every event whose time falls in [blockStart, blockStart +
  // numFrames), in time order, then removes them from the queue. Called once per render
  // quantum from the audio thread.
  template <typename Fn>
  void drainDue(int64_t blockStart, int32_t numFrames, Fn&& fn) {
    const int64_t blockEnd = blockStart + numFrames;
    size_t i = 0;
    for (; i < queue_.size() && queue_[i].atFrame < blockEnd; i++) {
      fn(queue_[i]);
    }
    queue_.erase(queue_.begin(), queue_.begin() + static_cast<long>(i));
  }

  size_t pending() const { return queue_.size(); }

 private:
  std::vector<ScheduledTrigger> queue_;
};

}  // namespace webdsp
