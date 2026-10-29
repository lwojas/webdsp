#pragma once
#include <cmath>
#include <cstdint>
#include "dsp/biquad_filter.h"  // kMaxDspChannels
#include "dsp/panner.h"
#include "dsp_node.h"
#include "fx_chain.h"
#include "params.h"

// A Bus accumulates voice output, runs it through its FX chain (fx_chain.h: an ordered list of
// independent effect instances, any kind, any number of each), and applies a final gain and
// pan stage before summing into whatever it routes to. Engine owns exactly one of these as the
// master bus plus a fixed pool as track buses (kMaxTrackBuses in engine.h); every track bus
// sums into the master bus before the master bus's own chain runs — see ARCHITECTURE.md,
// "Buses / mixing" and "FX chains". Track and master buses use this same class.
//
// Legacy NodeParam API: setParam(NodeParam::DelayMix, ...) etc. address the bus's "legacy"
// instance of that effect kind (id legacyFxId(kind)), created on first use and placed in the
// historical order filter, chorus/flanger, delay, reverb, compressor, saturation relative to
// the other legacy instances. A kind that was never touched is simply absent, which is exactly
// equivalent to the old always-present-but-inert node. Rationale for the historical order:
// chorus/flanger reads best on a still-dry signal, the compressor shapes dynamics before the
// saturator adds harmonic glue as the last stage.
namespace webdsp {

class Bus {
 public:
  void configure(double sampleRate) {
    chain_.configure(sampleRate);
    panner_ = Panner{};
    panner_.configure(sampleRate);
    everFed_ = false;
    configured_ = false;
    processed_ = false;
    gainApplied_ = gain_;
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) {
    processed_ = true;
    chain_.process(channels, numChannels, numFrames);
    applyGain(channels, numChannels, numFrames);
    // Bus pan is balance (the bus carries a stereo mix), after the output gain. Unity skips it.
    if (numChannels >= 2 && !panner_.isUnity()) {
      panner_.process(channels[0], channels[1], channels[0], channels[1], numFrames,
                      /*monoSource=*/false, /*accumulate=*/false);
    }
  }

  // Engine::process skips a dormant bus. A bus is dormant only while it has never been fed a voice
  // and nothing has been configured on it (an effect added, or any effect parameter set). Gain
  // doesn't count, since it multiplies zero. Every effect starts inert, so such a bus would emit
  // exact zeros. Once it is fed or configured, it runs every block, even when silent: silent
  // output doesn't prove a node's internal state has decayed (a compressor envelope can still be
  // ringing while its output is zero), and skipping on a level threshold changed the next sound
  // by up to 0.06 in testing. Saturation is the exception to zero-in, zero-out: with asymmetry
  // set it emits a DC-blocker transient even from silence, which is why configuring any effect
  // also ends dormancy.
  bool isDormant() const { return !everFed_ && !configured_; }
  void markFed() { everFed_ = true; }

  // --- generic FX chain (see fx_chain.h) ---
  bool addEffect(int32_t id, int32_t kind, int32_t index) {
    if (id <= 0) return false;  // negative ids are reserved for legacy instances
    if (!chain_.add(id, kind, index)) return false;
    configured_ = true;
    return true;
  }
  bool removeEffect(int32_t id) { return chain_.remove(id); }
  bool moveEffect(int32_t id, int32_t index) { return chain_.move(id, index); }
  bool setEffectEnabled(int32_t id, bool enabled) {
    if (!chain_.setEnabled(id, enabled)) return false;
    configured_ = true;
    return true;
  }
  bool setEffectParam(int32_t id, int32_t param, float value) {
    if (!chain_.setParam(id, param, value)) return false;
    configured_ = true;
    return true;
  }
  bool setEffectLfo(int32_t id, int32_t slot, const LfoConfig& cfg, double nowFrame) {
    if (!chain_.setLfo(id, slot, cfg, nowFrame)) return false;
    configured_ = true;
    return true;
  }
  void setClock(int64_t blockStartFrame, double anchorFrame, double framesPerBeat) {
    chain_.setClock(blockStartFrame, anchorFrame, framesPerBeat);
  }
  const FxChain& fx() const { return chain_; }
  float gainBase() const { return gain_; }
  bool addAnalysisTap(int32_t effectId, bool post, AnalysisTap* tap) { return chain_.addTap(effectId, post, tap); }
  void removeAnalysisTap(const AnalysisTap* tap) { chain_.removeTap(tap); }
  void resetFx() { chain_.reset(); }

  // NodeParam ids are a separate numeric space from VoiceParam (see params.h). Bus-level
  // BusGain/BusPan are handled here; every effect parameter is routed to the legacy instance of
  // its kind. Gain and pan only scale/redistribute the bus's signal (zero in, zero out), so they
  // don't end dormancy. Unknown ids are ignored.
  void setParam(int32_t param, float value) {
    if (param == static_cast<int32_t>(NodeParam::BusGain)) {
      if (!std::isfinite(value)) return;  // a malformed level must not reach the render path
      gain_ = value;
      // Before the bus has ever rendered there is nothing to glide from: start at the level.
      if (!processed_) gainApplied_ = value;
      return;
    }
    if (param == static_cast<int32_t>(NodeParam::BusPan)) {
      panner_.setPan(value);
      // Before the bus has ever rendered there is nothing to glide from: start at the position.
      if (!processed_) panner_.snap(/*monoSource=*/false);
      return;
    }
    const int32_t kind = fxKindForNodeParam(param);
    if (kind < 0) return;
    const int32_t id = legacyFxId(static_cast<FxKind>(kind));
    if (chain_.indexOf(id) < 0 && !addLegacy(id, kind)) return;
    chain_.setParam(id, param, value);
    configured_ = true;
  }

 private:
  // The bus fader (and a host's mute). A change glides linearly across the block it lands in, so a
  // mute or fader move never steps the signal (the same treatment as BusMainGain/BusSendAmount in
  // Engine::process and BusPan's smoothing). At rest it is exact, and unity skips the pass.
  void applyGain(float* const* channels, int32_t numChannels, int32_t numFrames) {
    const float from = gainApplied_;
    const float to = gain_;
    gainApplied_ = to;
    if (from == to) {
      if (to == 1.0f) return;
      for (int32_t c = 0; c < numChannels; c++) {
        for (int32_t i = 0; i < numFrames; i++) channels[c][i] *= to;
      }
      return;
    }
    const float step = (to - from) / static_cast<float>(numFrames);
    for (int32_t c = 0; c < numChannels; c++) {
      for (int32_t i = 0; i < numFrames; i++) {
        channels[c][i] *= from + step * static_cast<float>(i);
      }
    }
  }

  // Inserts the legacy instance before the first legacy instance of a later kind, else appends,
  // so legacy-only use reproduces the historical fixed order exactly.
  bool addLegacy(int32_t id, int32_t kind) {
    int32_t at = chain_.count();
    for (int32_t i = 0; i < chain_.count(); i++) {
      const int32_t other = chain_.idAt(i);
      if (other < 0 && (-1 - other) > kind) {
        at = i;
        break;
      }
    }
    return chain_.add(id, kind, at);
  }

  FxChain chain_;
  Panner panner_;
  float gain_ = 1.0f;
  float gainApplied_ = 1.0f;  // gain_ as of the end of the last block: where the next glide starts
  bool processed_ = false;
  bool everFed_ = false;
  bool configured_ = false;
};

}  // namespace webdsp
