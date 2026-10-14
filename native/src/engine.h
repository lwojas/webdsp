#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include "bus.h"
#include "capture.h"
#include "instrument_registry.h"
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
// `createBus()`), never a message round-trip to this thread. Sized for 4 banks of 16 pads
// (ECS-84). A bus's effect instances (incl. ~0.8 MB delay lines) are created on demand,
// so an unused bus is cheap. A bus that has never been fed is skipped per block
// (see Bus::isDormant), so unused buses cost no DSP. Mirrored by `MAX_TRACK_BUSES` in
// src/runtime/types.ts — keep in sync.
constexpr int32_t kMaxTrackBuses = 64;

// Upper bound on live instrument instances (like kMaxTrackBuses, a runtime-policy cap that
// keeps the render loop bounded, not a hardware fact).
constexpr int32_t kMaxInstruments = 64;

class Engine {
 public:
  void init(double sampleRate, int32_t outputChannels, int32_t maxVoices) {
    sampleRate_ = sampleRate;
    outputChannels_ = std::min(outputChannels, kMaxDspChannels);
    voices_.configure(sampleRate, maxVoices);
    masterBus_.configure(sampleRate);
    outputBuffers_.assign(outputChannels_, std::vector<float>(kMaxRenderQuantum, 0.0f));
    scratchBuffers_.assign(outputChannels_, std::vector<float>(kMaxRenderQuantum, 0.0f));
    endedVoices_.reserve(maxVoices * 2);
    pendingEnded_.reserve(maxVoices);

    trackBuses_ = std::vector<Bus>(kMaxTrackBuses);  // Bus is move-only (owns its FX instances)
    for (auto& bus : trackBuses_) bus.configure(sampleRate);
    trackBusBuffers_.assign(
        kMaxTrackBuses, std::vector<std::vector<float>>(outputChannels_,
                                                          std::vector<float>(kMaxRenderQuantum, 0.0f)));

    // Per-block pointer tables are built once here, so process() never allocates. The buffers
    // they point into are never resized after this point.
    busPtrs_.resize(outputChannels_);
    scratchPtrs_.resize(outputChannels_);
    for (int32_t c = 0; c < outputChannels_; c++) {
      busPtrs_[c] = outputBuffers_[c].data();
      scratchPtrs_[c] = scratchBuffers_[c].data();
    }
    trackPtrs_.assign(kMaxTrackBuses, std::vector<float*>(outputChannels_));
    for (size_t i = 0; i < trackPtrs_.size(); i++) {
      for (int32_t c = 0; c < outputChannels_; c++) trackPtrs_[i][c] = trackBusBuffers_[i][c].data();
    }
    busHasInput_.assign(kMaxTrackBuses, 0);
  }

  void reconfigureVoices(int32_t maxVoices) { voices_.configure(sampleRate_, maxVoices); }

  // --- samples ---
  // Replacing an id frees its old PCM, so any voice still reading it is stopped first.
  void commitSample(int32_t id, int32_t numChannels, int32_t length, int32_t sampleRate,
                     const int32_t* channelPtrs) {
    stopVoicesUsing(id);
    samples_.commit(id, numChannels, length, sampleRate, channelPtrs);
  }
  void commitSample(int32_t id, int32_t numChannels, int32_t length, int32_t sampleRate,
                     float* const* channelData) {
    stopVoicesUsing(id);
    samples_.commitOwned(id, numChannels, length, sampleRate, channelData);
  }
  // Frees the PCM only after every voice reading it has been stopped, so no render can touch
  // freed memory. Each stopped voice is reported through endedVoices() on the next process(),
  // like any other voice that finished, so the host can clear its own state for that voice.
  void removeSample(int32_t id) {
    stopVoicesUsing(id);
    samples_.remove(id);
  }
  // SampleParam::Gain / Pan / envelope — see params.h's doc comment and Voice::render for where it's
  // actually read. A reference to an unknown/removed sample id is a silent no-op (SampleStore
  // ::setParam), the same malformed-command policy every other setter here follows.
  void setSampleParam(int32_t sampleId, int32_t param, float value) {
    samples_.setParam(sampleId, param, value);
  }

  // --- immediate voice control ---
  int32_t trigger(int32_t voiceId, int32_t sampleId, int32_t busId, float gain, float rate,
                   int32_t startFrame, int32_t endFrame, bool loop, bool reverse,
                   int32_t durationFrames = -1, float pan = 0.0f) {
    const Sample* sample = samples_.get(sampleId);
    if (!sample) return voiceId;  // unknown or already-removed sample: nothing to play
    voices_.trigger(voiceId, sample, busId, gain, rate, startFrame, endFrame, loop, reverse,
                     /*delayFrames=*/0, durationFrames, pan);
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

  // --- FX chains (see fx_chain.h; busId addressing as setBusParam) ---
  // Structure and parameter commands for a bus's ordered effect chain. `effectId` is chosen by
  // the caller (> 0, unique per bus). All return false, changing nothing, on a bad bus, unknown
  // id, duplicate id, unknown kind, or a full chain.
  bool addEffect(int32_t busId, int32_t effectId, int32_t kind, int32_t index = -1) {
    Bus* b = busFor(busId);
    return b && b->addEffect(effectId, kind, index);
  }
  bool removeEffect(int32_t busId, int32_t effectId) {
    Bus* b = busFor(busId);
    return b && b->removeEffect(effectId);
  }
  bool moveEffect(int32_t busId, int32_t effectId, int32_t index) {
    Bus* b = busFor(busId);
    return b && b->moveEffect(effectId, index);
  }
  bool setEffectEnabled(int32_t busId, int32_t effectId, bool enabled) {
    Bus* b = busFor(busId);
    return b && b->setEffectEnabled(effectId, enabled);
  }
  bool setEffectParam(int32_t busId, int32_t effectId, int32_t param, float value) {
    Bus* b = busFor(busId);
    return b && b->setEffectParam(effectId, param, value);
  }
  const FxChain* effectChain(int32_t busId) const {
    const Bus* b = busFor(busId);
    return b ? &b->fx() : nullptr;
  }

  // --- instruments (synthesized sources; see instrument.h and ARCHITECTURE.md) ---
  // An instrument instance is created once (allocates — message thread, never process()) and
  // then addressed by caller-chosen `instrumentId` (> 0, unique). It renders into `busId` like a
  // sample voice, so track/master FX apply to it unchanged. Returns false on a bad id, unknown
  // kind, duplicate id, or full pool.
  bool createInstrument(int32_t instrumentId, int32_t kind, int32_t busId) {
    if (instrumentId <= 0 || findInstrument(instrumentId) ||
        static_cast<int32_t>(instruments_.size()) >= kMaxInstruments) {
      return false;
    }
    std::unique_ptr<Instrument> inst = makeInstrument(kind);
    if (!inst) return false;
    inst->configure(sampleRate_);
    instruments_.push_back({instrumentId, busId, std::move(inst)});
    return true;
  }
  bool removeInstrument(int32_t instrumentId) {
    for (size_t i = 0; i < instruments_.size(); i++) {
      if (instruments_[i].id != instrumentId) continue;
      instruments_.erase(instruments_.begin() + static_cast<long>(i));
      return true;
    }
    return false;
  }
  void instrumentNoteOn(int32_t instrumentId, float velocity, float pan = 0.0f) {
    if (InstrumentSlot* s = findInstrument(instrumentId)) s->inst->noteOn(velocity, 0, pan);
  }
  void instrumentNoteOff(int32_t instrumentId) {
    if (InstrumentSlot* s = findInstrument(instrumentId)) s->inst->noteOff();
  }
  void setInstrumentParam(int32_t instrumentId, int32_t param, float value) {
    if (InstrumentSlot* s = findInstrument(instrumentId)) s->inst->setParam(param, value);
  }
  void scheduleInstrumentNote(int64_t atFrame, int32_t instrumentId, float velocity,
                              float pan = 0.0f) {
    instrumentScheduler_.push({atFrame, instrumentId, velocity, pan});
  }
  int32_t instrumentCount() const { return static_cast<int32_t>(instruments_.size()); }
  int32_t activeInstrumentCount() const {
    int32_t n = 0;
    for (const auto& s : instruments_) n += s.inst->isActive() ? 1 : 0;
    return n;
  }

  // --- scheduling ---
  void scheduleEvent(int64_t atFrame, int32_t voiceId, int32_t sampleId, int32_t busId,
                      float gain, float rate, int32_t startFrame, int32_t endFrame, bool loop,
                      bool reverse, int32_t durationFrames = -1, float pan = 0.0f) {
    scheduler_.push({atFrame, voiceId, sampleId, busId, gain, rate, startFrame, endFrame, loop,
                      reverse, durationFrames, pan});
  }
  void cancelScheduled(int64_t fromFrame) {
    scheduler_.cancelFrom(fromFrame);
    instrumentScheduler_.cancelFrom(fromFrame);
  }

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
    endedVoices_.insert(endedVoices_.end(), pendingEnded_.begin(), pendingEnded_.end());
    pendingEnded_.clear();
    finishedCaptures_.clear();

    for (int32_t c = 0; c < outputChannels_; c++) {
      std::fill(outputBuffers_[c].begin(), outputBuffers_[c].begin() + numFrames, 0.0f);
    }
    // Track bus accumulators are zeroed lazily, only when a voice first renders into them (or
    // when a non-dormant bus needs silent input), so an idle bus is never touched here.
    std::fill(busHasInput_.begin(), busHasInput_.end(), 0);

    scheduler_.drainDue(blockStartFrame, numFrames, [&](const ScheduledTrigger& t) {
      const Sample* sample = samples_.get(t.sampleId);
      if (!sample) return;  // removed after it was scheduled: drop it rather than steal a voice
      const int32_t delay =
          static_cast<int32_t>(std::max<int64_t>(0, t.atFrame - blockStartFrame));
      voices_.trigger(t.voiceId, sample, t.busId, t.gain, t.rate, t.startFrame, t.endFrame,
                       t.loop, t.reverse, delay, t.durationFrames, t.pan);
    });

    instrumentScheduler_.drainDue(blockStartFrame, numFrames, [&](const InstrumentNote& n) {
      InstrumentSlot* s = findInstrument(n.instrumentId);
      if (!s) return;  // removed after it was scheduled
      const int32_t delay =
          static_cast<int32_t>(std::max<int64_t>(0, n.atFrame - blockStartFrame));
      s->inst->noteOn(n.velocity, delay, n.pan);
    });

    // A voice routed to busId N renders into its bus's own accumulator instead of the master
    // accumulator, so each bus's filter/delay only ever sees that bus's own voices. See
    // "Buses / mixing" in ARCHITECTURE.md. Sample voices and instruments share this routing.
    auto destFor = [&](int32_t busId) {
      float* const* dest = busPtrs_.data();
      if (busId >= 1 && busId <= static_cast<int32_t>(trackBuses_.size())) {
        const size_t i = static_cast<size_t>(busId - 1);
        if (!busHasInput_[i]) {
          zeroTrackBus(i, numFrames);
          busHasInput_[i] = 1;
        }
        dest = trackPtrs_[i].data();
      }
      return dest;
    };

    voices_.forEachActive([&](Voice& v) {
      const bool stillActive =
          v.render(destFor(v.busId()), outputChannels_, numFrames, scratchPtrs_.data());
      if (!stillActive) endedVoices_.push_back(v.id());
    });

    // Instruments render only while sounding, so an idle one never wakes a dormant bus.
    for (auto& s : instruments_) {
      if (!s.inst->isActive()) continue;
      s.inst->render(destFor(s.busId), outputChannels_, numFrames, scratchPtrs_.data());
    }

    // Each track bus runs its own filter+delay chain, then sums (the "mixer") into the
    // master accumulator, which finally runs masterBus_'s own chain — Track FX -> Mixer ->
    // Master FX -> Output, matching the application-level signal flow this exists to serve.
    // A bus that has never been fed contributes nothing and holds no state, so it is skipped.
    for (size_t i = 0; i < trackBuses_.size(); i++) {
      Bus& bus = trackBuses_[i];
      if (busHasInput_[i]) {
        bus.markFed();
      } else if (bus.isDormant()) {
        continue;
      } else {
        zeroTrackBus(i, numFrames);  // silent input: any delay or reverb tail keeps ringing
      }
      bus.process(trackPtrs_[i].data(), outputChannels_, numFrames);
      for (int32_t c = 0; c < outputChannels_; c++) {
        float* dst = outputBuffers_[c].data();
        const float* src = trackBusBuffers_[i][c].data();
        for (int32_t f = 0; f < numFrames; f++) dst[f] += src[f];
      }
    }

    masterBus_.process(busPtrs_.data(), outputChannels_, numFrames);

    for (auto& [id, cap] : captures_) {
      if (!cap.isActive()) continue;
      cap.appendBlock(busPtrs_.data(), outputChannels_, numFrames, blockStartFrame);
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
  // Stops every voice still reading sample `id`. Runs outside process() (from the message
  // handler), so it is never concurrent with rendering.
  void stopVoicesUsing(int32_t id) {
    const Sample* sample = samples_.get(id);
    if (!sample) return;
    voices_.forEachActive([&](Voice& v) {
      if (!v.usesSample(sample)) return;
      v.stop();
      pendingEnded_.push_back(v.id());
    });
  }

  struct InstrumentSlot {
    int32_t id;
    int32_t busId;
    std::unique_ptr<Instrument> inst;
  };
  struct InstrumentNote {
    int64_t atFrame;
    int32_t instrumentId;
    float velocity;
    float pan;
  };
  InstrumentSlot* findInstrument(int32_t id) {
    for (auto& s : instruments_) {
      if (s.id == id) return &s;
    }
    return nullptr;
  }

  // busId <= 0 is MASTER_BUS; 1..kMaxTrackBuses are track buses; anything else is null.
  Bus* busFor(int32_t busId) {
    if (busId <= 0) return &masterBus_;
    return busId <= static_cast<int32_t>(trackBuses_.size()) ? &trackBuses_[busId - 1] : nullptr;
  }
  const Bus* busFor(int32_t busId) const { return const_cast<Engine*>(this)->busFor(busId); }

  void zeroTrackBus(size_t i, int32_t numFrames) {
    for (int32_t c = 0; c < outputChannels_; c++) {
      std::fill(trackBusBuffers_[i][c].begin(), trackBusBuffers_[i][c].begin() + numFrames, 0.0f);
    }
  }

  double sampleRate_ = 48000.0;
  int32_t outputChannels_ = 2;

  SampleStore samples_;
  VoiceManager voices_;
  Bus masterBus_;
  std::vector<Bus> trackBuses_;
  Scheduler scheduler_;
  EventQueue<InstrumentNote> instrumentScheduler_;
  std::vector<InstrumentSlot> instruments_;
  std::unordered_map<int32_t, Capture> captures_;

  std::vector<std::vector<float>> outputBuffers_;
  std::vector<std::vector<float>> scratchBuffers_;
  std::vector<std::vector<std::vector<float>>> trackBusBuffers_;
  std::vector<float*> busPtrs_;
  std::vector<float*> scratchPtrs_;
  std::vector<std::vector<float*>> trackPtrs_;
  std::vector<uint8_t> busHasInput_;
  std::vector<int32_t> endedVoices_;
  std::vector<int32_t> pendingEnded_;
  std::vector<int32_t> finishedCaptures_;
};

}  // namespace webdsp
