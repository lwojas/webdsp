#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include "dsp/biquad_filter.h"
#include "dsp/bitcrusher.h"
#include "dsp/chorus_flanger.h"
#include "dsp/compressor.h"
#include "dsp/delay.h"
#include "dsp/phaser.h"
#include "dsp/reverb.h"
#include "dsp/saturation.h"
#include "dsp_node.h"
#include "params.h"

// FxChain: an ordered list of independent effect *instances* (any kind, any number of each),
// the generic mechanism behind every Bus's DSP. See ARCHITECTURE.md, "FX chains".
//
// Threading: structure (add/remove/move/enable) and parameter changes run from the command
// handler, which on the AudioWorklet is the same thread as process() but never concurrent with
// it. Instances are heap-allocated there, never in process(). process() walks a fixed array of
// already-resolved node pointers: no lookup by id or kind, no allocation, no branching on kind.
namespace webdsp {

// Mirrors src/runtime/types.ts EffectKind.
enum class FxKind : int32_t {
  Filter = 0,
  ChorusFlanger = 1,
  Delay = 2,
  Reverb = 3,
  Compressor = 4,
  Saturation = 5,
  Phaser = 6,
  Bitcrusher = 7,
};
constexpr int32_t kFxKindCount = 8;

// Per-bus instance cap. Each Delay holds ~0.8 MB and a Reverb more, so this bounds worst-case
// memory per bus while being far above any practical chain.
constexpr int32_t kMaxFxPerBus = 16;

// Instance ids chosen by the caller are > 0 and unique within a bus. Negative ids are reserved
// for the bus's own "legacy" instances created by NodeParam writes (see Bus::setParam):
// legacyFxId(kind) = -1 - kind.
constexpr int32_t legacyFxId(FxKind k) { return -1 - static_cast<int32_t>(k); }

// Which effect a NodeParam id belongs to, or -1 if it is not an effect parameter (BusGain,
// BusPan, unknown). NodeParam ids are partitioned by effect, see params.h.
inline int32_t fxKindForNodeParam(int32_t p) {
  using N = NodeParam;
  auto in = [p](N lo, N hi) { return p >= static_cast<int32_t>(lo) && p <= static_cast<int32_t>(hi); };
  if (in(N::DelayTime, N::DelayMix)) return static_cast<int32_t>(FxKind::Delay);
  if (in(N::FilterCutoff, N::FilterMode) || p == static_cast<int32_t>(N::FilterSlope))
    return static_cast<int32_t>(FxKind::Filter);
  if (in(N::ReverbDecay, N::ReverbMix)) return static_cast<int32_t>(FxKind::Reverb);
  if (in(N::CompThreshold, N::CompMakeup)) return static_cast<int32_t>(FxKind::Compressor);
  if (in(N::SatDrive, N::SatMix)) return static_cast<int32_t>(FxKind::Saturation);
  if (in(N::ChorusFlangerRate, N::ChorusFlangerMix)) return static_cast<int32_t>(FxKind::ChorusFlanger);
  if (in(N::PhaserRate, N::PhaserMix)) return static_cast<int32_t>(FxKind::Phaser);
  if (in(N::CrushBits, N::CrushMix)) return static_cast<int32_t>(FxKind::Bitcrusher);
  return -1;
}

class FxChain {
 public:
  void configure(double sampleRate) {
    clear();
    sampleRate_ = sampleRate;
  }

  int32_t count() const { return count_; }
  int32_t idAt(int32_t i) const { return inRange(i) ? slots_[i].id : 0; }
  int32_t kindAt(int32_t i) const { return inRange(i) ? static_cast<int32_t>(slots_[i].kind) : -1; }
  bool enabledAt(int32_t i) const { return inRange(i) && slots_[i].enabled; }
  int32_t indexOf(int32_t id) const {
    for (int32_t i = 0; i < count_; i++) {
      if (slots_[i].id == id) return i;
    }
    return -1;
  }

  // Inserts a new instance of `kind` at `index` (clamped; <0 or >= count appends). Fails (false)
  // on a duplicate id, an unknown kind, or a full chain; nothing changes in that case.
  bool add(int32_t id, int32_t kind, int32_t index) {
    if (id == 0 || kind < 0 || kind >= kFxKindCount || count_ >= kMaxFxPerBus || indexOf(id) >= 0) {
      return false;
    }
    const int32_t at = (index < 0 || index > count_) ? count_ : index;
    Slot& s = slots_[count_];
    s.id = id;
    s.kind = static_cast<FxKind>(kind);
    s.enabled = true;
    s.node = makeNode(s.kind, sampleRate_);
    count_++;
    std::rotate(slots_.begin() + at, slots_.begin() + count_ - 1, slots_.begin() + count_);
    return true;
  }

  // Destroys the instance; every other instance (same kind or not) keeps its state untouched.
  bool remove(int32_t id) {
    const int32_t i = indexOf(id);
    if (i < 0) return false;
    std::rotate(slots_.begin() + i, slots_.begin() + i + 1, slots_.begin() + count_);
    slots_[--count_] = Slot{};
    return true;
  }

  // Moves the instance to `newIndex` (clamped), shifting the ones in between. State is kept.
  bool move(int32_t id, int32_t newIndex) {
    const int32_t from = indexOf(id);
    if (from < 0) return false;
    const int32_t to = std::clamp(newIndex, 0, count_ - 1);
    if (from < to) {
      std::rotate(slots_.begin() + from, slots_.begin() + from + 1, slots_.begin() + to + 1);
    } else if (to < from) {
      std::rotate(slots_.begin() + to, slots_.begin() + from, slots_.begin() + from + 1);
    }
    return true;
  }

  // A disabled instance is skipped by process(). Disabling clears its internal state (tails,
  // envelopes) so re-enabling starts clean rather than replaying stale audio.
  bool setEnabled(int32_t id, bool enabled) {
    const int32_t i = indexOf(id);
    if (i < 0) return false;
    Slot& s = slots_[i];
    if (s.enabled && !enabled) s.node->reset();
    s.enabled = enabled;
    return true;
  }

  // `param` is a NodeParam id. Ignored if it does not belong to the instance's kind.
  bool setParam(int32_t id, int32_t param, float value) {
    const int32_t i = indexOf(id);
    if (i < 0) return false;
    Slot& s = slots_[i];
    if (fxKindForNodeParam(param) != static_cast<int32_t>(s.kind)) return false;
    if (s.kind == FxKind::Filter) param = toVoiceFilterParam(param);
    s.node->setParam(param, value);
    return true;
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) {
    for (int32_t i = 0; i < count_; i++) {
      if (slots_[i].enabled) slots_[i].node->process(channels, numChannels, numFrames);
    }
  }

  void reset() {
    for (int32_t i = 0; i < count_; i++) slots_[i].node->reset();
  }

  void clear() {
    for (int32_t i = 0; i < count_; i++) slots_[i] = Slot{};
    count_ = 0;
  }

 private:
  struct Slot {
    int32_t id = 0;
    FxKind kind = FxKind::Filter;
    bool enabled = true;
    std::unique_ptr<DSPNode> node;
  };

  bool inRange(int32_t i) const { return i >= 0 && i < count_; }

  // BiquadFilter understands VoiceParam ids; NodeParam is a separate id space (see params.h).
  static int32_t toVoiceFilterParam(int32_t nodeParam) {
    switch (static_cast<NodeParam>(nodeParam)) {
      case NodeParam::FilterCutoff: return static_cast<int32_t>(VoiceParam::FilterCutoff);
      case NodeParam::FilterResonance: return static_cast<int32_t>(VoiceParam::FilterResonance);
      case NodeParam::FilterMode: return static_cast<int32_t>(VoiceParam::FilterMode);
      default: return static_cast<int32_t>(VoiceParam::FilterSlope);
    }
  }

  // The one place that knows the concrete effect classes. A new effect adds a kind and a case.
  static std::unique_ptr<DSPNode> makeNode(FxKind kind, double sr) {
    switch (kind) {
      case FxKind::Filter: return std::make_unique<BiquadFilter>(sr);
      case FxKind::ChorusFlanger: return std::make_unique<ChorusFlanger>(sr);
      case FxKind::Delay: return std::make_unique<Delay>(sr);
      case FxKind::Reverb: return std::make_unique<Reverb>(sr);
      case FxKind::Compressor: return std::make_unique<Compressor>(sr);
      case FxKind::Saturation: return std::make_unique<Saturation>(sr);
      case FxKind::Phaser: return std::make_unique<Phaser>(sr);
      case FxKind::Bitcrusher: return std::make_unique<Bitcrusher>(sr);
    }
    return nullptr;
  }

  std::array<Slot, kMaxFxPerBus> slots_{};
  int32_t count_ = 0;
  double sampleRate_ = 48000.0;
};

}  // namespace webdsp
