#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/fm_kernel.h"
#include "../dsp/matrix_filter.h"
#include "../dsp/percussion.h"

// Synthesized "Matrix": a semi-modular subtractive voice, in the spirit of a Moog Mother-32. Behaviour,
// parameter mapping and client notes: docs/matrix-instrument.md. An original design.
//
//   outs:  VCO, noise, LFO 1, LFO 2, ENV 1, ENV 2, VCF out, VCA out
//   ins:   VCO pitch, VCO pulse width, VCF in, VCF cutoff, VCF resonance, VCF drive,
//          VCA in, VCA level, LFO 1 rate, LFO 2 rate, OUT
//
// There is no fixed signal path. The modules (an oscillator, a filter, an amplifier, two LFOs, two
// envelopes, a noise source) are joined only by the matrix: 88 cells, one per (out, in) pair, each a
// signed amount that is an ordinary catalogue parameter (ids 21..108, matrixAmountParam()). Whatever is
// patched into an in is summed, each out scaled by its amount, so an in with several cables is a small
// mixer. The ordinary voice is the default patch, pre-wired the way a Mother-32 is normalled:
// VCO > VCF in, VCF > VCA in, VCA > OUT, ENV 1 > VCA level, ENV 2 > cutoff. Remove or redirect any of
// those like any other cable (the oscillator straight into the amp, noise through the filter, the
// filter back into its own input).
//
// What an in does with its sum: VCO pitch moves the pitch (100% = 1 octave), pulse width +-0.45,
// VCF in is the filter's audio input, cutoff 5 octaves, resonance +-0.5, drive +-0.6, VCA in is the
// amp's audio input, VCA level opens the amp (0..1, then squared), LFO rates move the rate (100% = 3
// octaves), OUT is the voice output. The knobs set where each in rests before the cables.
//
// A loop (VCF out > cutoff, VCF out > pitch, VCO out > its own pitch, ...) is well defined: each sample
// the modules are computed in the order of the outs above (VCO, VCF, VCA) so an in hears a downstream out
// in the same sample and an upstream out (or its own module) one sample late. Everything is bounded: the
// filter limits its signal to +-1 and the output is soft-clipped to exactly +-1, so no patch can run away.
//
// Reused DSP: AdsrEnvelope (both envelopes), PercCore (lifecycle/pan/routing), PercNoise, the fmk sine
// table (LFOs), setTableParam. New: dsp/matrix_filter.h (per-sample retunable filter).
//
// Mono per instance; a retrigger is continuous (phases, filter state and envelope levels carry); a fresh
// note resets everything so identical inputs render identical samples. ENV 1 sets how long a note lives (a
// note ends when ENV 1 has released). Not tempo-synced. No allocation after configure().
namespace webdsp {

inline constexpr InstrumentParamInfo kMatrixParams[] = {
    {static_cast<int32_t>(MatrixParam::Tune), "tune", 27.5f, 1760.0f, 110.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(MatrixParam::Wave), "wave", 0.0f, 2.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(MatrixParam::Pwm), "pwm", 0.05f, 0.95f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Cutoff), "cutoff", 20.0f, 18000.0f, 1200.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(MatrixParam::Resonance), "resonance", 0.0f, 1.0f, 0.25f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Drive), "drive", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Mode), "mode", 0.0f, 3.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(MatrixParam::Keytrack), "keytrack", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Level), "level", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Lfo1Rate), "lfo1rate", 0.05f, 40.0f, 2.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(MatrixParam::Lfo1Wave), "lfo1wave", 0.0f, 4.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(MatrixParam::Lfo2Rate), "lfo2rate", 0.05f, 40.0f, 0.5f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(MatrixParam::Lfo2Wave), "lfo2wave", 0.0f, 4.0f, 1.0f, "step", ModDomain::None},
    {static_cast<int32_t>(MatrixParam::Env1Attack), "env1attack", 0.0005f, 4.0f, 0.003f, "s"},
    {static_cast<int32_t>(MatrixParam::Env1Decay), "env1decay", 0.01f, 4.0f, 0.4f, "s"},
    {static_cast<int32_t>(MatrixParam::Env1Sustain), "env1sustain", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(MatrixParam::Env1Release), "env1release", 0.005f, 4.0f, 0.12f, "s"},
    {static_cast<int32_t>(MatrixParam::Env2Attack), "env2attack", 0.0005f, 4.0f, 0.002f, "s"},
    {static_cast<int32_t>(MatrixParam::Env2Decay), "env2decay", 0.01f, 4.0f, 0.25f, "s"},
    {static_cast<int32_t>(MatrixParam::Env2Sustain), "env2sustain", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(MatrixParam::Env2Release), "env2release", 0.005f, 4.0f, 0.12f, "s"},
    // The matrix: id = AmountBase + out * 11 + in. -1..1, 0 = no cable. Named "<out>_<in>".
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::Pitch), "vco_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::Pwm), "vco_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::VcfIn), "vco_vcfin", -1.0f, 1.0f, 1.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::Cutoff), "vco_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::Resonance), "vco_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::Drive), "vco_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::VcaIn), "vco_vcain", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::Level), "vco_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::Lfo1Rate), "vco_lfo1rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::Lfo2Rate), "vco_lfo2rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vco, MatrixIn::Out), "vco_out", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::Pitch), "noise_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::Pwm), "noise_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::VcfIn), "noise_vcfin", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::Cutoff), "noise_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::Resonance), "noise_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::Drive), "noise_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::VcaIn), "noise_vcain", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::Level), "noise_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::Lfo1Rate), "noise_lfo1rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::Lfo2Rate), "noise_lfo2rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Noise, MatrixIn::Out), "noise_out", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::Pitch), "lfo1_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::Pwm), "lfo1_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::VcfIn), "lfo1_vcfin", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::Cutoff), "lfo1_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::Resonance), "lfo1_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::Drive), "lfo1_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::VcaIn), "lfo1_vcain", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::Level), "lfo1_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::Lfo1Rate), "lfo1_lfo1rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::Lfo2Rate), "lfo1_lfo2rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo1, MatrixIn::Out), "lfo1_out", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::Pitch), "lfo2_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::Pwm), "lfo2_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::VcfIn), "lfo2_vcfin", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::Cutoff), "lfo2_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::Resonance), "lfo2_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::Drive), "lfo2_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::VcaIn), "lfo2_vcain", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::Level), "lfo2_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::Lfo1Rate), "lfo2_lfo1rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::Lfo2Rate), "lfo2_lfo2rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Lfo2, MatrixIn::Out), "lfo2_out", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::Pitch), "env1_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::Pwm), "env1_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::VcfIn), "env1_vcfin", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::Cutoff), "env1_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::Resonance), "env1_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::Drive), "env1_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::VcaIn), "env1_vcain", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::Level), "env1_level", -1.0f, 1.0f, 1.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::Lfo1Rate), "env1_lfo1rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::Lfo2Rate), "env1_lfo2rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env1, MatrixIn::Out), "env1_out", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::Pitch), "env2_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::Pwm), "env2_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::VcfIn), "env2_vcfin", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::Cutoff), "env2_cutoff", -1.0f, 1.0f, 0.4f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::Resonance), "env2_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::Drive), "env2_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::VcaIn), "env2_vcain", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::Level), "env2_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::Lfo1Rate), "env2_lfo1rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::Lfo2Rate), "env2_lfo2rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Env2, MatrixIn::Out), "env2_out", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::Pitch), "vcf_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::Pwm), "vcf_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::VcfIn), "vcf_vcfin", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::Cutoff), "vcf_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::Resonance), "vcf_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::Drive), "vcf_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::VcaIn), "vcf_vcain", -1.0f, 1.0f, 1.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::Level), "vcf_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::Lfo1Rate), "vcf_lfo1rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::Lfo2Rate), "vcf_lfo2rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vcf, MatrixIn::Out), "vcf_out", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::Pitch), "vca_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::Pwm), "vca_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::VcfIn), "vca_vcfin", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::Cutoff), "vca_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::Resonance), "vca_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::Drive), "vca_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::VcaIn), "vca_vcain", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::Level), "vca_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::Lfo1Rate), "vca_lfo1rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::Lfo2Rate), "vca_lfo2rate", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixOut::Vca, MatrixIn::Out), "vca_out", -1.0f, 1.0f, 1.0f, "", ModDomain::None},
};
inline constexpr int32_t kMatrixParamCount =
    static_cast<int32_t>(sizeof(kMatrixParams) / sizeof(kMatrixParams[0]));
static_assert(kMatrixParamCount == static_cast<int32_t>(MatrixParam::AmountBase) + kMatrixAmountCount,
              "the table is the 21 module controls followed by every matrix cell, in id order");

class MatrixInstrument final : public Instrument {
 public:
  MatrixInstrument() {
    for (int32_t i = 0; i < kMatrixParamCount; i++) values_[i] = kMatrixParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Matrix; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    env1_.configure(sampleRate);
    env2_.configure(sampleRate);
    env1_.reset();
    env2_.reset();
    filter_.configure(sampleRate);
    sine_ = fmk::sineTable();
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    if (core_.begin(velocity, delayFrames, pan, pitch)) clearState();  // retrigger carries state
    env1_.noteOnRestart(envParams(0));
    env2_.noteOnRestart(envParams(1));
  }

  void duck(int32_t frames) override { env1_.duck(frames); }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    env1_.noteOff(envParams(0));
    env2_.noteOff(envParams(1));
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kMatrixParams, kMatrixParamCount, values_, param, v);
  }

  void reset() override {
    env1_.reset();
    env2_.reset();
    clearState();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* mono = scratch[0];
    const double sr = core_.sampleRate;

    const float levelGain = std::min(core_.velocity, 1.0f) * kLevel;
    const double incRest = static_cast<double>(value(MatrixParam::Tune)) * core_.pitchRatio / sr;
    const int32_t wave = static_cast<int32_t>(value(MatrixParam::Wave) + 0.5f);
    const int32_t mode = static_cast<int32_t>(value(MatrixParam::Mode) + 0.5f);
    const int32_t lfo1Wave = static_cast<int32_t>(value(MatrixParam::Lfo1Wave) + 0.5f);
    const int32_t lfo2Wave = static_cast<int32_t>(value(MatrixParam::Lfo2Wave) + 0.5f);
    const double lfo1Inc = static_cast<double>(value(MatrixParam::Lfo1Rate)) / sr;
    const double lfo2Inc = static_cast<double>(value(MatrixParam::Lfo2Rate)) / sr;
    const float keytrack = static_cast<float>(std::pow(core_.pitchRatio, static_cast<double>(value(MatrixParam::Keytrack))));

    // Pre-note delay: silence, and nothing advances.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t k = 0; k < lead; k++) mono[k] = 0.0f;
    core_.pendingDelay -= lead;

    bool finished = false;
    int32_t i = lead;
    while (i < numFrames) {
      const int32_t n = std::min(kSubBlock, numFrames - i);
      if (env1_.stage() == AdsrEnvelope::Stage::Idle ||
          (env1_.stage() == AdsrEnvelope::Stage::Sustain && env1_.level() <= 0.0f)) {
        finished = true;
        for (int32_t k = i; k < numFrames; k++) mono[k] = 0.0f;
        break;
      }

      // --- control rate: this step's cables (amounts glide so a change never clicks) and the knobs.
      gatherCables();
      const float pwmBase = value(MatrixParam::Pwm);
      const float cutBase = value(MatrixParam::Cutoff) * keytrack;
      const float resBase = value(MatrixParam::Resonance);
      const float driveBase = value(MatrixParam::Drive);
      const float levelBase = value(MatrixParam::Level);
      const bool pitchMod = count_[kPitch] > 0, cutMod = count_[kCutoff] > 0;
      float cutNow = cutBase;

      // --- audio rate, one module after another. `out_` holds each out's latest value, so an in
      // reading an out of a module not yet computed this sample gets the previous one.
      for (int32_t k = 0; k < n; k++) {
        out_[kNoise] = noise_.next();

        double inc1 = lfo1Inc, inc2 = lfo2Inc;
        if (count_[kLfo1Rate]) inc1 *= std::exp2(static_cast<double>(std::clamp(kLfoRateOct * sum(kLfo1Rate), -kMaxRateOct, kMaxRateOct)));
        if (count_[kLfo2Rate]) inc2 *= std::exp2(static_cast<double>(std::clamp(kLfoRateOct * sum(kLfo2Rate), -kMaxRateOct, kMaxRateOct)));
        out_[kLfo1] = lfo1_.next(lfo1Wave, std::min(inc1, kLfoIncCap), sine_);
        out_[kLfo2] = lfo2_.next(lfo2Wave, std::min(inc2, kLfoIncCap), sine_);
        out_[kEnv1] = env1_.next();
        out_[kEnv2] = env2_.next();

        const float pw = std::clamp(pwmBase + kPwmDepth * sum(kPwm), 0.02f, 0.98f);
        double inc = incRest;
        if (pitchMod) inc *= std::exp2(static_cast<double>(std::clamp(sum(kPitch), -kMaxPitchOct, kMaxPitchOct)));
        out_[kVco] = oscillator(wave, std::min(inc, kIncCap), pw);

        if (cutMod) cutNow = cutBase * std::exp2(std::clamp(kCutoffOct * sum(kCutoff), -kMaxCutoffOct, kMaxCutoffOct));
        const float res = count_[kRes] ? std::clamp(resBase + kResDepth * sum(kRes), 0.0f, 1.0f) : resBase;
        const float drv = count_[kDrive] ? std::clamp(driveBase + kDriveDepth * sum(kDrive), 0.0f, 1.0f) : driveBase;
        out_[kVcf] = filter_.process(sum(kVcfIn), cutNow, res, drv, mode);

        const float gain = std::clamp(levelBase + sum(kLevelIn), 0.0f, 1.0f);
        out_[kVca] = sum(kVcaIn) * gain * gain;

        mono[i + k] = softClip(sum(kOut) * levelGain);
      }
      i += n;
    }

    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr int32_t kVco = static_cast<int32_t>(MatrixOut::Vco);
  static constexpr int32_t kNoise = static_cast<int32_t>(MatrixOut::Noise);
  static constexpr int32_t kLfo1 = static_cast<int32_t>(MatrixOut::Lfo1);
  static constexpr int32_t kLfo2 = static_cast<int32_t>(MatrixOut::Lfo2);
  static constexpr int32_t kEnv1 = static_cast<int32_t>(MatrixOut::Env1);
  static constexpr int32_t kEnv2 = static_cast<int32_t>(MatrixOut::Env2);
  static constexpr int32_t kVcf = static_cast<int32_t>(MatrixOut::Vcf);
  static constexpr int32_t kVca = static_cast<int32_t>(MatrixOut::Vca);
  static constexpr int32_t kPitch = static_cast<int32_t>(MatrixIn::Pitch);
  static constexpr int32_t kPwm = static_cast<int32_t>(MatrixIn::Pwm);
  static constexpr int32_t kVcfIn = static_cast<int32_t>(MatrixIn::VcfIn);
  static constexpr int32_t kCutoff = static_cast<int32_t>(MatrixIn::Cutoff);
  static constexpr int32_t kRes = static_cast<int32_t>(MatrixIn::Resonance);
  static constexpr int32_t kDrive = static_cast<int32_t>(MatrixIn::Drive);
  static constexpr int32_t kVcaIn = static_cast<int32_t>(MatrixIn::VcaIn);
  static constexpr int32_t kLevelIn = static_cast<int32_t>(MatrixIn::Level);
  static constexpr int32_t kLfo1Rate = static_cast<int32_t>(MatrixIn::Lfo1Rate);
  static constexpr int32_t kLfo2Rate = static_cast<int32_t>(MatrixIn::Lfo2Rate);
  static constexpr int32_t kOut = static_cast<int32_t>(MatrixIn::Out);

  static constexpr float kLevel = 0.9f;
  static constexpr int32_t kSubBlock = 32;        // control-rate step (~0.67 ms at 48 kHz)
  static constexpr float kAmountGlide = 0.35f;    // per step; settles within a few ms
  static constexpr double kIncCap = 0.45;         // instantaneous frequency < 0.45 fs
  static constexpr double kLfoIncCap = 0.25;
  // What a 100% cable does (an amount a moves the in by a x out x depth).
  static constexpr float kMaxPitchOct = 6.0f;     // pitch: 100% = 1 octave
  static constexpr float kPwmDepth = 0.45f;       // pulse width: 100% = +-0.45
  static constexpr float kCutoffOct = 5.0f;       // cutoff: 100% = 5 octaves
  static constexpr float kMaxCutoffOct = 9.0f;
  static constexpr float kResDepth = 0.5f;        // resonance: 100% = +-0.5
  static constexpr float kDriveDepth = 0.6f;      // drive: 100% = +-0.6
  static constexpr float kLfoRateOct = 3.0f;      // LFO rate: 100% = 3 octaves
  static constexpr float kMaxRateOct = 9.0f;

  static int32_t idx(MatrixParam p) { return static_cast<int32_t>(p); }
  float value(MatrixParam p) const { return values_[idx(p)]; }

  AdsrParams envParams(int32_t env) const {
    const MatrixParam a = env == 0 ? MatrixParam::Env1Attack : MatrixParam::Env2Attack;
    const int32_t b = idx(a);
    return AdsrParams{values_[b], values_[b + 1], values_[b + 2], values_[b + 3]};
  }

  // Steps the glided amounts toward the parameter values and rebuilds each in's list of live cables (so
  // an unpatched in costs nothing per sample).
  void gatherCables() {
    const int32_t base = idx(MatrixParam::AmountBase);
    for (int32_t in = 0; in < kMatrixInCount; in++) {
      int32_t n = 0;
      for (int32_t o = 0; o < kMatrixOutCount; o++) {
        const float target = values_[base + o * kMatrixInCount + in];
        float& cur = amount_[o][in];
        cur = snapAmounts_ ? target : cur + (target - cur) * kAmountGlide;
        if (target == 0.0f && std::fabs(cur) < 1e-4f) cur = 0.0f;
        if (cur != 0.0f) {
          cableOut_[in][n] = o;
          cableAmt_[in][n] = cur;
          n++;
        }
      }
      count_[in] = n;
    }
    snapAmounts_ = false;
  }

  // Everything patched into `in`, each out scaled by its amount.
  float sum(int32_t in) const {
    float t = 0.0f;
    for (int32_t j = 0; j < count_[in]; j++) t += cableAmt_[in][j] * out_[cableOut_[in][j]];
    return t;
  }

  // Band-limited (PolyBLEP) saw and pulse; the triangle is naive (its harmonics fall as 1/n^2).
  float oscillator(int32_t wave, double inc, float width) {
    phase_ += inc;
    phase_ -= std::floor(phase_);
    const double p = phase_;
    if (wave == 1) {
      double q = p - static_cast<double>(width);
      q -= std::floor(q);
      const float s = (p < width ? 1.0f : -1.0f) + polyBlep(p, inc) - polyBlep(q, inc);
      return s - (2.0f * width - 1.0f);  // remove the DC a narrow pulse carries
    }
    if (wave == 2) return static_cast<float>(1.0 - 4.0 * std::fabs(p - 0.5));
    return static_cast<float>(2.0 * p - 1.0) - polyBlep(p, inc);
  }

  static float polyBlep(double t, double dt) {
    if (t < dt) {
      const double x = t / dt;
      return static_cast<float>(x + x - x * x - 1.0);
    }
    if (t > 1.0 - dt) {
      const double x = (t - 1.0) / dt;
      return static_cast<float>(x * x + x + x + 1.0);
    }
    return 0.0f;
  }

  void clearState() {
    phase_ = 0.0;
    for (float& o : out_) o = 0.0f;
    lfo1_.reset(0x2545F491u);
    lfo2_.reset(0x9E3779B1u);
    noise_.reset();
    filter_.reset();
    for (auto& row : amount_) for (float& a : row) a = 0.0f;
    snapAmounts_ = true;
  }

  // Transparent below 0.6, then a smooth knee that is bounded by exactly 1.
  static float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.6f) return x;
    const float u = std::min((a - 0.6f) * 2.5f, 3.0f);
    const float y = 0.6f + 0.4f * (u * (27.0f + u * u) / (27.0f + 9.0f * u * u));
    const float c = std::min(y, 1.0f);
    return x < 0.0f ? -c : c;
  }

  // Free-running bipolar LFO; sample & hold takes a new value each cycle.
  struct Lfo {
    double phase = 0.0;
    float held = 0.0f;
    uint32_t rng = 1;
    void reset(uint32_t seed) {
      phase = 0.0;
      rng = seed;
      held = draw();
    }
    float draw() {
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      return static_cast<float>(static_cast<int32_t>(rng)) * (1.0f / 2147483648.0f);
    }
    float next(int32_t wave, double inc, const float* sine) {
      const double p = phase;
      phase += inc;
      if (phase >= 1.0) {
        phase -= std::floor(phase);
        held = draw();
      }
      switch (wave) {
        case 0: return fmk::sinTurns(sine, p);
        case 1: return static_cast<float>(p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0));
        case 2: return static_cast<float>(2.0 * p - 1.0);
        case 3: return p < 0.5 ? 1.0f : -1.0f;
        default: return held;
      }
    }
  };

  float values_[kMatrixParamCount] = {};
  PercCore core_;
  AdsrEnvelope env1_, env2_;
  MatrixFilter filter_;
  PercNoise noise_;
  Lfo lfo1_, lfo2_;
  const float* sine_ = nullptr;
  double phase_ = 0.0;
  float out_[kMatrixOutCount] = {};
  float amount_[kMatrixOutCount][kMatrixInCount] = {};
  float cableAmt_[kMatrixInCount][kMatrixOutCount] = {};
  int32_t cableOut_[kMatrixInCount][kMatrixOutCount] = {};
  int32_t count_[kMatrixInCount] = {};
  bool snapAmounts_ = true;
};

}  // namespace webdsp
