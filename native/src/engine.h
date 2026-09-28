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

// Additional buses beyond MASTER_BUS (busId 0), for an application to give each of its own
// channels (e.g. a sequencer track) an independent filter+delay chain before the mix — see
// ARCHITECTURE.md, "Buses / mixing". Pre-allocated in full at init() (like the voice pool)
// so handing one out is just a counter bump on the JS side (webdsp/src/runtime/index.ts
// `createBus()`), never a message round-trip to this thread: a fresh Bus is a no-op
// pass-through (bypassed filter, zero delay mix — see BiquadFilter/Delay defaults) until
// parameterized, so processing an unused one costs one cheap early-out per node, not a
// per-sample loop. Mirrored by `MAX_TRACK_BUSES` in src/runtime/types.ts — keep in sync.
constexpr int32_t kMaxTrackBuses = 32;

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

    trackBuses_.assign(kMaxTrackBuses, Bus{});
    for (auto& bus : trackBuses_) bus.configure(sampleRate);
    trackBusBuffers_.assign(
        kMaxTrackBuses, std::vector<std::vector<float>>(outputChannels_,
                                                          std::vector<float>(kMaxRenderQuantum, 0.0f)));
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
  // busId 0 (MASTER_BUS) addresses masterBus_; 1..kMaxTrackBuses address trackBuses_[busId-1].
  // An out-of-range busId is silently ignored, matching the engine's existing policy that a
  // malformed command must not corrupt render-thread state (see api.cpp / engine-processor.ts).
  void setBusParam(int32_t busId, int32_t param, float value) {
    if (busId <= 0) {
      masterBus_.setParam(param, value);
    } else if (busId <= static_cast<int32_t>(trackBuses_.size())) {
      trackBuses_[busId - 1].setParam(param, value);
    }
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
  // Arms a sample-accurate capture of [startFrame, stopFrame) — see Capture::arm(). `busId`
  // is accepted for symmetry with startCapture() but, like startCapture(), is not yet used:
  // process() always taps the post-masterBus_ output for every capture, which is already the
  // "track FX -> mixer -> master FX" tap point an application-level resampler needs. Returns
  // false if the window was refused (see Capture::arm()'s doc comment).
  bool armCapture(int32_t captureId, int32_t /*busId*/, int64_t startFrame, int64_t stopFrame) {
    return captures_[captureId].arm(outputChannels_, sampleRate_, startFrame, stopFrame);
  }
  const Capture* capture(int32_t captureId) const {
    auto it = captures_.find(captureId);
    return it == captures_.end() ? nullptr : &it->second;
  }
  void discardCapture(int32_t captureId) { captures_.erase(captureId); }
  static constexpr double maxCaptureSeconds() { return kMaxCaptureSeconds; }
  // Ids of captures that reached their armed stopFrame during the most recent process() call
  // — cleared and repopulated once per block, same pattern as endedVoices(). A manual
  // (non-armed) capture never appears here; the caller is expected to stopCapture() it.
  const std::vector<int32_t>& finishedCaptures() const { return finishedCaptures_; }

  // --- realtime render ---
  void process(int64_t blockStartFrame, int32_t numFrames) {
    numFrames = std::min(numFrames, kMaxRenderQuantum);
    endedVoices_.clear();
    finishedCaptures_.clear();

    std::vector<float*> busPtrs(outputChannels_);
    std::vector<float*> scratchPtrs(outputChannels_);
    for (int32_t c = 0; c < outputChannels_; c++) {
      std::fill(outputBuffers_[c].begin(), outputBuffers_[c].begin() + numFrames, 0.0f);
      busPtrs[c] = outputBuffers_[c].data();
      scratchPtrs[c] = scratchBuffers_[c].data();
    }

    // One pointer table per track bus, into that bus's own accumulator (zeroed below) — a
    // voice routed to busId N renders into trackPtrs[N-1] instead of the master accumulator,
    // so each bus's filter/delay only ever sees that bus's own voices. See "Buses / mixing"
    // in ARCHITECTURE.md.
    std::vector<std::vector<float*>> trackPtrs(trackBuses_.size(), std::vector<float*>(outputChannels_));
    for (size_t i = 0; i < trackBusBuffers_.size(); i++) {
      for (int32_t c = 0; c < outputChannels_; c++) {
        std::fill(trackBusBuffers_[i][c].begin(), trackBusBuffers_[i][c].begin() + numFrames, 0.0f);
        trackPtrs[i][c] = trackBusBuffers_[i][c].data();
      }
    }

    scheduler_.drainDue(blockStartFrame, numFrames, [&](const ScheduledTrigger& t) {
      const int32_t delay =
          static_cast<int32_t>(std::max<int64_t>(0, t.atFrame - blockStartFrame));
      const Sample* sample = samples_.get(t.sampleId);
      voices_.trigger(t.voiceId, sample, t.busId, t.gain, t.rate, t.startFrame, t.endFrame,
                       t.loop, t.reverse, delay, t.durationFrames);
    });

    voices_.forEachActive([&](Voice& v) {
      const int32_t busId = v.busId();
      float* const* dest = (busId >= 1 && busId <= static_cast<int32_t>(trackPtrs.size()))
                                ? trackPtrs[busId - 1].data()
                                : busPtrs.data();
      const bool stillActive = v.render(dest, outputChannels_, numFrames, scratchPtrs.data());
      if (!stillActive) endedVoices_.push_back(v.id());
    });

    // Each track bus runs its own filter+delay chain, then sums (the "mixer") into the
    // master accumulator, which finally runs masterBus_'s own chain — Track FX -> Mixer ->
    // Master FX -> Output, matching the application-level signal flow this exists to serve.
    for (size_t i = 0; i < trackBuses_.size(); i++) {
      trackBuses_[i].process(trackPtrs[i].data(), outputChannels_, numFrames);
      for (int32_t c = 0; c < outputChannels_; c++) {
        float* dst = outputBuffers_[c].data();
        const float* src = trackBusBuffers_[i][c].data();
        for (int32_t f = 0; f < numFrames; f++) dst[f] += src[f];
      }
    }

    masterBus_.process(busPtrs.data(), outputChannels_, numFrames);

    for (auto& [id, cap] : captures_) {
      if (!cap.isActive()) continue;
      cap.appendBlock(busPtrs.data(), outputChannels_, numFrames, blockStartFrame);
      if (cap.isFinished()) finishedCaptures_.push_back(id);
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
  std::vector<Bus> trackBuses_;
  Scheduler scheduler_;
  std::unordered_map<int32_t, Capture> captures_;

  std::vector<std::vector<float>> outputBuffers_;
  std::vector<std::vector<float>> scratchBuffers_;
  std::vector<std::vector<std::vector<float>>> trackBusBuffers_;
  std::vector<int32_t> endedVoices_;
  std::vector<int32_t> finishedCaptures_;
};

}  // namespace webdsp
