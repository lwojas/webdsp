#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Scheduled parameter events: a timestamped write of a parameter's BASE value, with an optional
// linear ramp. The queue is the same EventQueue notes use (scheduler.h); this header holds the event
// type and the fixed-size ramp table. Design, timing rationale and the phase-2 option:
// docs/scheduled-params.md.
namespace webdsp {

// What a parameter event addresses. Same ids the immediate setters take.
enum class ParamTarget : int32_t {
  Effect = 0,      // bus + effect id + NodeParam
  Instrument = 1,  // instrument id + the synth's param id
  Sample = 2,      // sample id + SampleParam
  Bus = 3,         // bus (in `bus`) + a mixer NodeParam: BusGain, BusPan, BusSendAmount, BusMainGain
};
inline constexpr int32_t kParamTargetCount = 4;

struct ParamEvent {
  int64_t atFrame;
  int32_t target;  // ParamTarget
  int32_t bus;     // Effect and Bus
  int32_t id;      // effect id / instrument id / sample id
  int32_t param;
  float value;     // the value reached (after `rampFrames`, or immediately if 0)
  float from;      // ramp start value, used only when hasFrom
  bool hasFrom;    // false: a ramp starts from the parameter's current base value
  int32_t rampFrames;
};

struct ParamKey {
  int32_t target;
  int32_t bus;
  int32_t id;
  int32_t param;
  bool operator==(const ParamKey& o) const {
    return target == o.target && bus == o.bus && id == o.id && param == o.param;
  }
};

inline constexpr int32_t kMaxParamRamps = 64;

// The active ramps. At most one per key (a new event for a key replaces its ramp). Fixed capacity,
// no allocation. Values are linear in the parameter's own units, the contract automation lanes
// already have, so an application that draws a lane as its breakpoints draws exactly this.
class ParamRamps {
 public:
  // Starts (or replaces) the ramp for `key`. False if the table is full, in which case the caller
  // applies the end value directly and the drop is counted.
  bool start(const ParamKey& key, int64_t startFrame, int32_t frames, float from, float to) {
    Ramp* slot = find(key);
    if (!slot) {
      for (Ramp& r : ramps_) {
        if (!r.active) {
          slot = &r;
          break;
        }
      }
    }
    if (!slot) {
      dropped_++;
      return false;
    }
    *slot = Ramp{true, key, startFrame, std::max(1, frames), from, to, from};
    return true;
  }

  void cancel(const ParamKey& key) {
    if (Ramp* r = find(key)) r->active = false;
  }

  // Drops every ramp of one target (an effect, instrument or sample that was removed).
  void cancelTarget(int32_t target, int32_t bus, int32_t id) {
    for (Ramp& r : ramps_) {
      if (r.active && r.key.target == target && r.key.bus == bus && r.key.id == id) r.active = false;
    }
  }

  // Evaluates every active ramp at `frame` (the first frame of the block being rendered) and calls
  // `apply(key, value)` for those whose value changed; a ramp that has reached its end applies its
  // exact end value and is removed. `apply` returns false if the target no longer exists, which
  // also removes the ramp.
  template <typename Apply>
  void advance(int64_t frame, Apply&& apply) {
    for (Ramp& r : ramps_) {
      if (!r.active) continue;
      const int64_t end = r.start + r.frames;
      float v;
      if (frame >= end) {
        v = r.to;
        r.active = false;
      } else {
        const double t = std::clamp(static_cast<double>(frame - r.start) / r.frames, 0.0, 1.0);
        v = static_cast<float>(r.from + (static_cast<double>(r.to) - r.from) * t);
      }
      if (v == r.last && r.active) continue;
      r.last = v;
      if (!apply(r.key, v)) r.active = false;
    }
  }

  int32_t active() const {
    int32_t n = 0;
    for (const Ramp& r : ramps_) n += r.active ? 1 : 0;
    return n;
  }
  int32_t dropped() const { return dropped_; }
  void clear() {
    for (Ramp& r : ramps_) r.active = false;
    dropped_ = 0;
  }

 private:
  struct Ramp {
    bool active = false;
    ParamKey key{};
    int64_t start = 0;
    int32_t frames = 1;
    float from = 0.0f, to = 0.0f, last = 0.0f;
  };

  Ramp* find(const ParamKey& key) {
    for (Ramp& r : ramps_) {
      if (r.active && r.key == key) return &r;
    }
    return nullptr;
  }

  Ramp ramps_[kMaxParamRamps];
  int32_t dropped_ = 0;
};

}  // namespace webdsp
