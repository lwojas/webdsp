#pragma once
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

// Owns decoded PCM for every loaded sample. Planar float32 per channel, allocated on the
// WASM heap so pointers can be handed straight to a Voice's render loop with no copying at
// trigger time — decode happens once, at load, on the JS side (see src/runtime/decode.ts);
// this store only ever receives already-decoded PCM. Mutation (load/unload) only ever
// happens from the worklet's message handler, never concurrently with rendering, because
// both run on the same single audio-rendering thread — see ARCHITECTURE.md, "Threading
// model", for why no locking is needed here.
namespace webdsp {

struct Sample {
  int32_t channels = 0;
  int32_t length = 0;       // frames
  int32_t sampleRate = 0;
  std::vector<float*> channelData;  // heap pointers, owned by this Sample
};

class SampleStore {
 public:
  ~SampleStore() {
    for (auto& [id, sample] : samples_) {
      freeSample(sample);
    }
  }

  void commit(int32_t id, int32_t numChannels, int32_t length, int32_t sampleRate,
              const int32_t* channelPtrs) {
    auto it = samples_.find(id);
    if (it != samples_.end()) {
      freeSample(it->second);
      samples_.erase(it);
    }
    Sample sample;
    sample.channels = numChannels;
    sample.length = length;
    sample.sampleRate = sampleRate;
    sample.channelData.resize(numChannels);
    for (int32_t c = 0; c < numChannels; c++) {
      sample.channelData[c] = reinterpret_cast<float*>(static_cast<intptr_t>(channelPtrs[c]));
    }
    samples_.emplace(id, std::move(sample));
  }

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
