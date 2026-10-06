#pragma once
#include <cstdint>
#include "dsp/biquad_filter.h"  // kMaxDspChannels
#include "dsp/chorus_flanger.h"
#include "dsp/compressor.h"
#include "dsp/delay.h"
#include "dsp/reverb.h"
#include "dsp/saturation.h"
#include "dsp_node.h"
#include "params.h"

// A Bus accumulates voice output, runs it through its own DSP chain — a filter
// (BiquadFilter, the same node type Voice uses), a chorus/flanger (dsp/chorus_flanger.h), a
// delay/send, a reverb (dsp/reverb.h), a compressor (dsp/compressor.h), and a saturator
// (dsp/saturation.h) — and applies a final gain stage before summing into whatever it routes
// to. Engine owns exactly one of these as the master bus plus a fixed pool as track buses
// (kMaxTrackBuses in engine.h); every track bus sums into the master bus before the master
// bus's own chain runs — see ARCHITECTURE.md, "Buses / mixing". Placement: the chorus/flanger
// runs right after tone-shaping (filter) and before the time-based/dynamics/harmonic stages —
// a modulated pitch/comb effect reads best on a still-dry signal, not on top of delay/reverb
// tails or post-compression dynamics. The compressor runs before the saturator (last in the
// chain): dynamics are shaped first, then the saturator adds harmonic "glue"/warmth to the
// fully-processed signal, the conventional position for a final-stage saturator/exciter in a
// mastering-style chain — matches the compressor's own "runs after everything else already
// there" placement one node further down.
namespace webdsp {

class Bus {
 public:
  void configure(double sampleRate) {
    filter_ = BiquadFilter(sampleRate);
    chorusFlanger_ = ChorusFlanger(sampleRate);
    delay_ = Delay(sampleRate);
    reverb_ = Reverb(sampleRate);
    compressor_ = Compressor(sampleRate);
    saturation_ = Saturation(sampleRate);
    chain_ = DSPChain<6>{};
    chain_.add(&filter_);
    chain_.add(&chorusFlanger_);
    chain_.add(&delay_);
    chain_.add(&reverb_);
    chain_.add(&compressor_);
    chain_.add(&saturation_);
    everFed_ = false;
    configured_ = false;
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) {
    chain_.process(channels, numChannels, numFrames);
    for (int32_t c = 0; c < numChannels; c++) {
      for (int32_t i = 0; i < numFrames; i++) channels[c][i] *= gain_;
    }
  }

  // Engine::process skips a dormant bus. A bus is dormant only while it has never been fed a voice
  // and no DSP parameter has been set on it. Gain doesn't count, since it multiplies zero. Every
  // node starts bypassed, so such a bus would emit exact zeros. Once it is fed or configured, it
  // runs every block, even when silent: silent output doesn't prove a node's internal state has
  // decayed (a compressor envelope can still be ringing while its output is zero), and skipping
  // on a level threshold changed the next sound by up to 0.06 in testing. Saturation is the
  // exception to zero-in, zero-out: with asymmetry set it emits a DC-blocker transient even from
  // silence, which is why configuring any DSP parameter also ends dormancy.
  bool isDormant() const { return !everFed_ && !configured_; }
  void markFed() { everFed_ = true; }

  // NodeParam ids are a separate numeric space from VoiceParam (see params.h), so the
  // master filter's params are translated to the VoiceParam ids BiquadFilter itself
  // understands rather than forwarded raw — the same DSPNode class, two independent
  // callers with independent id spaces.
  void setParam(int32_t param, float value) {
    if (param != static_cast<int32_t>(NodeParam::BusGain)) configured_ = true;
    if (param == static_cast<int32_t>(NodeParam::BusGain)) {
      gain_ = value;
    } else if (param == static_cast<int32_t>(NodeParam::FilterCutoff)) {
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), value);
    } else if (param == static_cast<int32_t>(NodeParam::FilterResonance)) {
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), value);
    } else if (param == static_cast<int32_t>(NodeParam::FilterMode)) {
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterMode), value);
    } else if (param == static_cast<int32_t>(NodeParam::ChorusFlangerRate) ||
               param == static_cast<int32_t>(NodeParam::ChorusFlangerDepth) ||
               param == static_cast<int32_t>(NodeParam::ChorusFlangerDelay) ||
               param == static_cast<int32_t>(NodeParam::ChorusFlangerFeedback) ||
               param == static_cast<int32_t>(NodeParam::ChorusFlangerStereoPhase) ||
               param == static_cast<int32_t>(NodeParam::ChorusFlangerMix)) {
      chorusFlanger_.setParam(param, value);
    } else if (param == static_cast<int32_t>(NodeParam::ReverbDecay) ||
               param == static_cast<int32_t>(NodeParam::ReverbDamping) ||
               param == static_cast<int32_t>(NodeParam::ReverbMix)) {
      reverb_.setParam(param, value);
    } else if (param == static_cast<int32_t>(NodeParam::CompThreshold) ||
               param == static_cast<int32_t>(NodeParam::CompRatio) ||
               param == static_cast<int32_t>(NodeParam::CompAttack) ||
               param == static_cast<int32_t>(NodeParam::CompRelease) ||
               param == static_cast<int32_t>(NodeParam::CompKnee) ||
               param == static_cast<int32_t>(NodeParam::CompMakeup)) {
      compressor_.setParam(param, value);
    } else if (param == static_cast<int32_t>(NodeParam::SatDrive) ||
               param == static_cast<int32_t>(NodeParam::SatAsymmetry) ||
               param == static_cast<int32_t>(NodeParam::SatOutputGain) ||
               param == static_cast<int32_t>(NodeParam::SatMix)) {
      saturation_.setParam(param, value);
    } else {
      delay_.setParam(param, value);
    }
  }

 private:
  BiquadFilter filter_{48000.0};
  ChorusFlanger chorusFlanger_{48000.0};
  Delay delay_{48000.0};
  Reverb reverb_{48000.0};
  Compressor compressor_{48000.0};
  Saturation saturation_{48000.0};
  DSPChain<6> chain_;
  float gain_ = 1.0f;
  bool everFed_ = false;
  bool configured_ = false;
};

}  // namespace webdsp
