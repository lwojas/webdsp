#pragma once
#include <algorithm>
#include <cstdint>
#include "../dsp/fm_kernel.h"
#include "../dsp/percussion.h"
#include "../instrument.h"

// The shared lifecycle of every Digital Machine, as a class template over a Machine policy
// (instruments/dm_*.h). Docs: docs/digital-machines.md.
//
// The template owns what is identical for all of them: the parameter value array, PercCore (velocity,
// pitch offset, pan, pre-note delay, active / releasing), the 16-sample control-rate sub-block loop,
// the output soft-clip and bus routing. The policy owns the sound: its param table, its operators
// and envelopes, its wiring. Because the policy is a template argument, its per-sample code is
// inlined into render(); there is no virtual call or indirection inside the audio loop (only the
// one Instrument::render per block, as for every instrument).
//
// A policy provides:
//   static constexpr InstrumentKind kKind;
//   static constexpr const InstrumentParamInfo* kParams;  static constexpr int32_t kParamCount;
//       (table order == id order; values[] is indexed by id)
//   void configure(double sampleRate, const float* sineTable);
//   void noteOn(const float* values, float velocity, double pitchRatio, bool fresh);
//   void noteOff(const float* values);
//   void reset();
//   bool render(const float* values, float* out, int32_t n);  // writes all n samples; false once finished
namespace webdsp {

template <class M>
class DigitalMachine final : public Instrument {
 public:
  DigitalMachine() {
    for (int32_t i = 0; i < M::kParamCount; i++) values_[i] = M::kParams[i].def;
  }

  InstrumentKind kind() const override { return M::kKind; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    m_.configure(sampleRate, fmk::sineTable());
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    const bool fresh = core_.begin(velocity, delayFrames, pan, pitch);
    m_.noteOn(values_, core_.velocity, core_.pitchRatio, fresh);
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    m_.noteOff(values_);
  }

  void setParam(int32_t param, float v) override {
    setTableParam(M::kParams, M::kParamCount, values_, param, v);
  }

  void reset() override {
    m_.reset();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* mono = scratch[0];

    // Pre-note delay: silence, and nothing advances.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t k = 0; k < lead; k++) mono[k] = 0.0f;
    core_.pendingDelay -= lead;

    bool finished = false;
    int32_t i = lead;
    while (i < numFrames) {
      const int32_t n = std::min(kSubBlock, numFrames - i);
      if (!m_.render(values_, mono + i, n)) {
        finished = true;
        for (int32_t k = i + n; k < numFrames; k++) mono[k] = 0.0f;
        break;
      }
      i += n;
    }

    for (int32_t k = lead; k < numFrames; k++) mono[k] = fmk::softClip(mono[k]);
    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr int32_t kSubBlock = 16;  // control-rate step (~0.33 ms at 48 kHz)

  float values_[M::kParamCount] = {};
  PercCore core_;
  M m_;
};

}  // namespace webdsp
