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
  // >= 0: auto-release this many frames after playback starts (note duration). -1: no
  // auto-release, matches pre-existing behavior. Trailing + defaulted so existing
  // aggregate-init call sites that don't mention it are unaffected.
  int32_t durationFrames = -1;
  // Stereo position, -1..+1 (see dsp/panner.h). Trailing + defaulted like durationFrames.
  float pan = 0.0f;
  // >= 0: chop playback — start/end come from this cue region of the sample, resolved when the
  // event fires. -1: whole-sample/chromatic (startFrame/endFrame used as given).
  int32_t region = -1;
  // > 0: the voice is a member of this choke group (see VoiceManager::trigger). 0: none.
  int32_t chokeGroup = 0;
};

// Time-ordered queue of any event type with an `int64_t atFrame` member. The sample-trigger
// queue below and the instrument note queue (instrument.h) are two instantiations.
template <typename Event>
class EventQueue {
 public:
  void push(const Event& event) {
    // Insertion-sorted by time; the queue is small (a UI/sequencer's lookahead window, not
    // an entire song) so this is simpler and cheap enough to be preferable to a heap here.
    auto it = std::upper_bound(queue_.begin(), queue_.end(), event,
                                [](const Event& a, const Event& b) {
                                  return a.atFrame < b.atFrame;
                                });
    queue_.insert(it, event);
  }

  void cancelFrom(int64_t fromFrame) {
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                 [&](const Event& e) { return e.atFrame >= fromFrame; }),
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
  std::vector<Event> queue_;
};

using Scheduler = EventQueue<ScheduledTrigger>;

}  // namespace webdsp
