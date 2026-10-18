#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include "dsp/adsr_envelope.h"
#include "params.h"

// Owns decoded PCM for every loaded sample. Planar float32 per channel, allocated on the
// WASM heap so pointers can be handed straight to a Voice's render loop with no copying at
// trigger time — decode happens once, at load, on the JS side (see src/runtime/decode.ts);
// this store only ever receives already-decoded PCM. Mutation (load/unload) only ever
// happens from the worklet's message handler, never concurrently with rendering, because
// both run on the same single audio-rendering thread — see ARCHITECTURE.md, "Threading
// model", for why no locking is needed here.
namespace webdsp {

// A named-by-index [start, end) span of a sample's frames (a cue point pair). Describes the
// audio, so it lives on the Sample; whether a voice plays one is decided per trigger (the
// `region` argument of Engine::trigger / scheduleEvent), never stored here. No audio is copied.
struct SampleRegion {
  int32_t start = 0;
  int32_t end = 0;  // exclusive
};

// Upper bound on regions per sample. Bounds the memory a malformed command can claim; regions
// are only ever edited from the message handler, so the vector may allocate there.
constexpr int32_t kMaxSampleRegions = 256;

struct Sample {
  int32_t channels = 0;
  int32_t length = 0;       // frames
  int32_t sampleRate = 0;
  std::vector<float*> channelData;  // heap pointers, owned by this Sample
  // Persistent per-sample gain (SampleParam::Gain) — see params.h's doc comment on
  // SampleParam. Defaults to unity so a sample that's never had its gain touched renders
  // exactly as before this existed. Survives commitOwned() replacing the same id's PCM? No —
  // a re-commit is a brand-new Sample value (see commit()/commitOwned() below), the same way
  // a replaced sample's voices are already stopped rather than migrated; a caller that wants
  // the gain to persist across a reload re-applies it after the new commit, exactly as it
  // already must re-apply anything else sample-specific.
  float gain = 1.0f;
  // Amplitude ADSR (SampleParam::EnvAttack..EnvRelease). Voices read this at note-on and at
  // each stage boundary, so edits apply to the next stage, not retroactively.
  AdsrParams envelope;
  // Instrument default pan (SampleParam::Pan), -1..+1; summed with the voice pan at render.
  float pan = 0.0f;
  // Cue regions in index order (empty = none; existing samples never have any). Read at trigger
  // time only, via regionBounds(); allocation-free.
  std::vector<SampleRegion> regions;

  // Resolves region `index` to [start, end) frames. False if there is no such region or it is
  // empty — the engine then drops the trigger rather than playing something unintended.
  bool regionBounds(int32_t index, int32_t& start, int32_t& end) const {
    if (index < 0 || index >= static_cast<int32_t>(regions.size())) return false;
    start = regions[index].start;
    end = regions[index].end;
    return end > start;
  }
};

class SampleStore {
 public:
  ~SampleStore() {
    for (auto& [id, sample] : samples_) {
      freeSample(sample);
    }
  }

  // WASM entry point: channel pointers arrive as 32-bit wasm32 addresses.
  void commit(int32_t id, int32_t numChannels, int32_t length, int32_t sampleRate,
              const int32_t* channelPtrs) {
    std::vector<float*> ptrs(numChannels);
    for (int32_t c = 0; c < numChannels; c++) {
      ptrs[c] = reinterpret_cast<float*>(static_cast<intptr_t>(channelPtrs[c]));
    }
    commitOwned(id, numChannels, length, sampleRate, ptrs.data());
  }

  // Native-pointer form of commit(), used by native tests and benchmarks where pointers do not
  // fit in 32 bits. Takes ownership of each channel buffer (freed with std::free).
  void commitOwned(int32_t id, int32_t numChannels, int32_t length, int32_t sampleRate,
                   float* const* channelData) {
    auto it = samples_.find(id);
    if (it != samples_.end()) {
      freeSample(it->second);
      samples_.erase(it);
    }
    Sample sample;
    sample.channels = numChannels;
    sample.length = length;
    sample.sampleRate = sampleRate;
    sample.channelData.assign(channelData, channelData + numChannels);
    samples_.emplace(id, std::move(sample));
  }

  // Frees the sample's PCM. The caller must first stop any voice still reading it (see
  // Engine::removeSample) — this store does not know about voices.
  void remove(int32_t id) {
    auto it = samples_.find(id);
    if (it == samples_.end()) return;
    freeSample(it->second);
    samples_.erase(it);
  }

  const Sample* get(int32_t id) const {
    auto it = samples_.find(id);
    return it == samples_.end() ? nullptr : &it->second;
  }

  // Mirrors Voice::setParam/Bus::setParam's shape (a flat param-id dispatch), even though
  // SampleParam has exactly one member today — the same "generic setter, small enum" pattern
  // the other two addressable things in this engine already use, so a future SampleParam
  // addition doesn't need a new ABI shape. A reference to an unknown/already-removed id is a
  // silent no-op, matching every other host-facing setter in this engine (a malformed/late
  // command must not be able to corrupt render-thread state).
  void setParam(int32_t id, int32_t param, float value) {
    auto it = samples_.find(id);
    if (it == samples_.end()) return;
    if (param == static_cast<int32_t>(SampleParam::Gain)) {
      it->second.gain = value;
    } else if (param == static_cast<int32_t>(SampleParam::Pan)) {
      if (std::isfinite(value)) it->second.pan = std::max(-1.0f, std::min(1.0f, value));
    } else if (param == static_cast<int32_t>(SampleParam::EnvAttack)) {
      it->second.envelope.attack = value;
    } else if (param == static_cast<int32_t>(SampleParam::EnvDecay)) {
      it->second.envelope.decay = value;
    } else if (param == static_cast<int32_t>(SampleParam::EnvSustain)) {
      it->second.envelope.sustain = value;
    } else if (param == static_cast<int32_t>(SampleParam::EnvRelease)) {
      it->second.envelope.release = value;
    }
    it->second.envelope = AdsrEnvelope::sanitize(it->second.envelope);
  }

  // Cue regions (see SampleRegion). Bounds are clamped into [0, length]; an unknown id or a
  // region beyond kMaxSampleRegions is a silent no-op like every other host-facing setter.
  void clearRegions(int32_t id) {
    auto it = samples_.find(id);
    if (it != samples_.end()) it->second.regions.clear();
  }
  void addRegion(int32_t id, int32_t start, int32_t end) {
    auto it = samples_.find(id);
    if (it == samples_.end()) return;
    Sample& s = it->second;
    if (static_cast<int32_t>(s.regions.size()) >= kMaxSampleRegions) return;
    start = std::max(0, std::min(start, s.length));
    end = std::max(start, std::min(end, s.length));
    s.regions.push_back({start, end});
  }

  int32_t count() const { return static_cast<int32_t>(samples_.size()); }

  int64_t totalBytes() const {
    int64_t bytes = 0;
    for (const auto& [id, sample] : samples_) {
      bytes += static_cast<int64_t>(sample.length) * sample.channels * sizeof(float);
    }
    return bytes;
  }

 private:
  static void freeSample(Sample& sample) {
    for (float* ptr : sample.channelData) std::free(ptr);
  }

  std::unordered_map<int32_t, Sample> samples_;
};

}  // namespace webdsp
