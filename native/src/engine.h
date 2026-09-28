#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "bus.h"
#include "capture.h"
#include "sample_store.h"
#include "scheduler.h"
#include "voice_manager.h"

// Top-level engine: owns every subsystem and is the only thing api.cpp talks to. Nothing
// here knows about pads, patterns, or any application concept — see ARCHITECTURE.md.
namespace webdsp {

// Real-world AudioWorklet render quanta are 128 frames on every current browser. Headroom
// is kept in case that ever changes; process() clamps to this regardless of what the host
// requests, which bounds every fixed-size buffer in the engine and keeps it allocation-free.
constexpr int32_t kMaxRenderQuantum = 512;

class Engine {
 public:
  void init(double sampleRate, int32_t outputChannels, int32_t maxVoices) {
    sampleRate_ = sampleRate;
    outputChannels_ = std::min(outputChannels, kMaxDspChannels);
    voices_.configure(sampleRate, maxVoices);
    masterBus_.configure(sampleRate);
    outputBuffers_.assign(outputChannels_, std::vector<float>(kMaxRenderQuantum, 0.0f));
    scratchBuffers_.assign(outputChannels_, std::vector<float>(kMaxRenderQuantum, 0.0f));
    endedVoices_.reserve(maxVoices);
  }

  void reconfigureVoices(int32_t maxVoices) { voices_.configure(sampleRate_, maxVoices); }

  // --- samples ---
  void commitSample(int32_t id, int32_t numChannels, int32_t length, int32_t sampleRate,
                     const int32_t* channelPtrs) {
    samples_.commit(id, numChannels, length, sampleRate, channelPtrs);
  }
  void removeSample(int32_t id) { samples_.remove(id); }

  // --- immediate voice control ---
  int32_t trigger(int32_t voiceId, int32_t sampleId, int32_t busId, float gain, float rate,
                   int32_t startFrame, int32_t endFrame, bool loop, bool reverse,
                   int32_t durationFrames = -1) {
    const Sample* sample = samples_.get(sampleId);
    voices_.trigger(voiceId, sample, busId, gain, rate, startFrame, endFrame, loop, reverse,
                     /*delayFrames=*/0, durationFrames);
    return voiceId;
  }
  void release(int32_t voiceId) { voices_.release(voiceId); }
  void stop(int32_t voiceId) { voices_.stop(voiceId); }
  void setVoiceParam(int32_t voiceId, int32_t param, float value) {
    if (Voice* v = voices_.find(voiceId)) v->setParam(param, value);
  }
  void setBusParam(int32_t /*busId*/, int32_t param, float value) {
    masterBus_.setParam(param, value);  // v1: single bus, see ARCHITECTURE.md "Buses"
  }

  // --- scheduling ---
  void scheduleEvent(int64_t atFrame, int32_t voiceId, int32_t sampleId, int32_t busId,
                      float gain, float rate, int32_t startFrame, int32_t endFrame, bool loop,
                      bool reverse, int32_t durationFrames = -1) {
    scheduler_.push({atFrame, voiceId, sampleId, busId, gain, rate, startFrame, endFrame, loop,
                      reverse, durationFrames});
  }
  void cancelScheduled(int64_t fromFrame) { scheduler_.cancelFrom(fromFrame); }

  // --- capture / resample proof of concept ---
  void startCapture(int32_t captureId, int32_t /*busId*/) {
    captures_[captureId].start(outputChannels_, sampleRate_);
  }
  void stopCapture(int32_t captureId) {
    auto it = captures_.find(captureId);
    if (it != captures_.end()) it->second.stop();
  }
  const Capture* capture(int32_t captureId) const {
    auto it = captures_.find(captureId);
    return it == captures_.end() ? nullptr : &it->second;
  }
  void discardCapture(int32_t captureId) { captures_.erase(captureId); }

  // --- realtime render ---
  void process(int64_t blockStartFrame, int32_t numFrames) {
    numFrames = std::min(numFrames, kMaxRenderQuantum);
    endedVoices_.clear();

    std::vector<float*> busPtrs(outputChannels_);
    std::vector<float*> scratchPtrs(outputChannels_);
    for (int32_t c = 0; c < outputChannels_; c++) {
      std::fill(outputBuffers_[c].begin(), outputBuffers_[c].begin() + numFrames, 0.0f);
      busPtrs[c] = outputBuffers_[c].data();
      scratchPtrs[c] = scratchBuffers_[c].data();
    }

    scheduler_.drainDue(blockStartFrame, numFrames, [&](const ScheduledTrigger& t) {
      const int32_t delay =
          static_cast<int32_t>(std::max<int64_t>(0, t.atFrame - blockStartFrame));
      const Sample* sample = samples_.get(t.sampleId);
      voices_.trigger(t.voiceId, sample, t.busId, t.gain, t.rate, t.startFrame, t.endFrame,
                       t.loop, t.reverse, delay, t.durationFrames);
    });

    voices_.forEachActive([&](Voice& v) {
      const bool stillActive = v.render(busPtrs.data(), outputChannels_, numFrames, scratchPtrs.data());
      if (!stillActive) endedVoices_.push_back(v.id());
    });

    masterBus_.process(busPtrs.data(), outputChannels_, numFrames);

    for (auto& [id, cap] : captures_) {
      if (cap.isActive()) cap.appendBlock(busPtrs.data(), outputChannels_, numFrames);
    }
  }

  float* outputChannel(int32_t c) { return outputBuffers_[c].data(); }

  // --- diagnostics ---
  int32_t activeVoiceCount() const { return voices_.activeCount(); }
  int32_t loadedSampleCount() const { return samples_.count(); }
  int64_t sampleMemoryBytes() const { return samples_.totalBytes(); }
  int32_t maxVoices() const { return voices_.maxVoices(); }
  double sampleRate() const { return sampleRate_; }
  int32_t outputChannels() const { return outputChannels_; }
  const std::vector<int32_t>& endedVoices() const { return endedVoices_; }

 private:
  double sampleRate_ = 48000.0;
  int32_t outputChannels_ = 2;

  SampleStore samples_;
  VoiceManager voices_;
  Bus masterBus_;
  Scheduler scheduler_;
  std::unordered_map<int32_t, Capture> captures_;

  std::vector<std::vector<float>> outputBuffers_;
  std::vector<std::vector<float>> scratchBuffers_;
  std::vector<int32_t> endedVoices_;
};

}  // namespace webdsp
