#pragma once
#include <cstdint>
#include "dsp/biquad_filter.h"  // kMaxDspChannels
#include "dsp/delay.h"
#include "dsp_node.h"
#include "params.h"

// A Bus accumulates voice output, runs it through its own DSP chain (v1: an optional
// delay/send, demonstrating the same DSPNode interface used per-voice for the filter), and
// applies a final gain stage before summing into whatever it routes to. v1 wires exactly
// one bus (master) straight to output; the structure supports more without any change to
// Voice or the DSPNode interface — see ARCHITECTURE.md, "Buses / mixing".
namespace webdsp {

class Bus {
 public:
  void configure(double sampleRate) {
    delay_ = Delay(sampleRate);
    chain_ = DSPChain<1>{};
    chain_.add(&delay_);
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) {
    chain_.process(channels, numChannels, numFrames);
    for (int32_t c = 0; c < numChannels; c++) {
      for (int32_t i = 0; i < numFrames; i++) channels[c][i] *= gain_;
    }
  }

  void setParam(int32_t param, float value) {
    if (param == static_cast<int32_t>(NodeParam::BusGain)) {
      gain_ = value;
    } else {
      delay_.setParam(param, value);
    }
  }

 private:
  Delay delay_{48000.0};
  DSPChain<1> chain_;
  float gain_ = 1.0f;
};

}  // namespace webdsp
