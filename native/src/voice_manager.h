#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include "sample_store.h"
#include "voice.h"

// Owns a pool of voices. This is the only place "how many voices can play at once" is
// decided, and it is a runtime policy (settable, growable), never a hardcoded pad/channel
// count — see ARCHITECTURE.md, "Voices, not channels".
namespace webdsp {

class VoiceManager {
 public:
  void configure(double sampleRate, int32_t maxVoices) {
    sampleRate_ = sampleRate;
    voices_.resize(std::max(1, maxVoices));
    for (auto& v : voices_) v.configure(sampleRate);
  }

  // Always succeeds: reuses an idle voice if one exists, otherwise steals whichever active
  // voice is currently quietest. `externalVoiceId` is caller-issued (see
  // ARCHITECTURE.md, "Voice handles") so triggering can return a usable handle
  // synchronously without a round trip back from the audio thread.
  //
  // `chokeGroup` > 0 first chokes (see chokeOthers) the group's other sources, then allocates as
  // usual. Choking is an explicit relationship and is not allocation: it happens whether or not an
  // idle voice is free, and allocation below neither knows nor cares about groups.
  Voice* trigger(int32_t externalVoiceId, const Sample* sample, int32_t busId, float gain,
                 float rate, int32_t startFrame, int32_t endFrame, bool loop, bool reverse,
                 int32_t delayFrames = 0, int32_t durationFrames = -1,
                 float pan = 0.0f, int32_t chokeGroup = 0) {
    if (chokeGroup > 0) chokeOthers(chokeGroup, busId, delayFrames);
    Voice* target = findIdle();
    if (!target) target = findStealCandidate();
    target->trigger(externalVoiceId, sample, busId, gain, rate, startFrame, endFrame, loop,
                     reverse, delayFrames, durationFrames, pan, chokeGroup);
    return target;
  }

  // Chokes every active voice (playing or in its release tail) that belongs to `group` and plays
  // into a different bus than `busId`, `atFrame` frames into the coming render. Voices on the
  // same bus are one source — a track's own overlapping voices are left to normal voice
  // behaviour. Returns how many voices were choked.
  int32_t chokeOthers(int32_t group, int32_t busId, int32_t atFrame) {
    int32_t n = 0;
    for (auto& v : voices_) {
      if (v.isIdle() || v.chokeGroup() != group || v.busId() == busId) continue;
      v.choke(atFrame);
      n++;
    }
    return n;
  }

  Voice* find(int32_t externalVoiceId) {
    for (auto& v : voices_) {
      if (!v.isIdle() && v.id() == externalVoiceId) return &v;
    }
    return nullptr;
  }

  void release(int32_t externalVoiceId) {
    if (Voice* v = find(externalVoiceId)) v->release();
  }

  void stop(int32_t externalVoiceId) {
    if (Voice* v = find(externalVoiceId)) v->stop();
  }

  template <typename Fn>
  void forEachActive(Fn&& fn) {
    for (auto& v : voices_) {
      if (!v.isIdle()) fn(v);
    }
  }

  int32_t activeCount() const {
    int32_t n = 0;
    for (const auto& v : voices_) {
      if (!v.isIdle()) n++;
    }
    return n;
  }

  int32_t maxVoices() const { return static_cast<int32_t>(voices_.size()); }

 private:
  Voice* findIdle() {
    for (auto& v : voices_) {
      if (v.isIdle()) return &v;
    }
    return nullptr;
  }

  // Steal policy: quietest voice by current amplitude estimate. Simple, cheap, and avoids
  // an audible pop from cutting the loudest thing playing. A more elaborate policy
  // (oldest-first, priority hints) is a reasonable future addition and does not require any
  // change to the public API — see ARCHITECTURE.md, "Intentionally deferred".
  Voice* findStealCandidate() {
    Voice* best = &voices_[0];
    float bestAmp = best->amplitudeEstimate();
    for (auto& v : voices_) {
      const float amp = v.amplitudeEstimate();
      if (amp < bestAmp) {
        best = &v;
        bestAmp = amp;
      }
    }
    return best;
  }

  double sampleRate_ = 48000.0;
  std::vector<Voice> voices_;
};

}  // namespace webdsp
