#pragma once
#include <cstdint>

// A DSP node is one stage in a signal-flow chain (voice -> node -> node -> bus -> master).
// This is the entire "effects" abstraction in the engine: there is no special-cased
// effects subsystem, just chains of these. See ARCHITECTURE.md, "DSP architecture".
namespace webdsp {

class DSPNode {
 public:
  virtual ~DSPNode() = default;

  // In-place processing of a planar buffer: `channels[c]` is `numFrames` samples for
  // channel c. Must not allocate; called from the audio render path.
  virtual void process(float* const* channels, int32_t numChannels, int32_t numFrames) = 0;

  virtual void setParam(int32_t param, float value) = 0;
  virtual void reset() = 0;
};

// Fixed-capacity, fixed-order chain of nodes. Insertion order is decided by whatever
// constructs the chain (Voice, Bus) — v1 does not expose reordering or dynamic insertion
// from the application; see ARCHITECTURE.md, "Intentionally deferred".
template <int32_t Capacity>
class DSPChain {
 public:
  void add(DSPNode* node) {
    if (count_ < Capacity) nodes_[count_++] = node;
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) {
    for (int32_t i = 0; i < count_; i++) {
      nodes_[i]->process(channels, numChannels, numFrames);
    }
  }

  void reset() {
    for (int32_t i = 0; i < count_; i++) nodes_[i]->reset();
  }

 private:
  DSPNode* nodes_[Capacity] = {};
  int32_t count_ = 0;
};

}  // namespace webdsp
