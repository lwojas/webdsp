#pragma once
#include <cstdint>
#include "dsp/biquad_filter.h"  // kMaxDspChannels
#include "dsp/delay.h"
#include "dsp_node.h"
#include "params.h"

// A Bus accumulates voice output, runs it through its own DSP chain — a filter
// (BiquadFilter, the same node type Voice uses) followed by an optional delay/send — and
// applies a final gain stage before summing into whatever it routes to. Engine owns exactly
// one of these as the master bus plus a fixed pool as track buses (kMaxTrackBuses in
// engine.h); every track bus sums into the master bus before the master bus's own chain
// runs — see ARCHITECTURE.md, "Buses / mixing".
namespace webdsp {

class Bus {
 public:
  void configure(double sampleRate) {
    filter_ = BiquadFilter(sampleRate);
    delay_ = Delay(sampleRate);
    chain_ = DSPChain<2>{};
    chain_.add(&filter_);
    chain_.add(&delay_);
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) {
    chain_.process(channels, numChannels, numFrames);
    for (int32_t c = 0; c < numChannels; c++) {
      for (int32_t i = 0; i < numFrames; i++) channels[c][i] *= gain_;
    }
  }

  // NodeParam ids are a separate numeric space from VoiceParam (see params.h), so the
  // master filter's params are translated to the VoiceParam ids BiquadFilter itself
  // understands rather than forwarded raw — the same DSPNode class, two independent
  // callers with independent id spaces.
  void setParam(int32_t param, float value) {
    if (param == static_cast<int32_t>(NodeParam::BusGain)) {
      gain_ = value;
    } else if (param == static_cast<int32_t>(NodeParam::FilterCutoff)) {
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), value);
    } else if (param == static_cast<int32_t>(NodeParam::FilterResonance)) {
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), value);
    } else if (param == static_cast<int32_t>(NodeParam::FilterMode)) {
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterMode), value);
    } else {
      delay_.setParam(param, value);
    }
  }

 private:
  BiquadFilter filter_{48000.0};
  Delay delay_{48000.0};
  DSPChain<2> chain_;
  float gain_ = 1.0f;
};

}  // namespace webdsp
