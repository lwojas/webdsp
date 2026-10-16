#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/biquad_filter.h"
#include "../dsp/percussion.h"
#include "../dsp/rhythm.h"

// Rhythmic synth: "Duck & Breathe". Design rationale: docs/pulse-instrument-proposal.md.
//
//   3x saw (PolyBLEP, detuned by diffuse) + sub sine (-1 oct)
//        -> asymmetric tanh, drive = base + repeat contour                     (pre-filter)
//        -> DC block (~5 Hz)
//        -> BiquadFilter LP 24 dB, cutoff = base * 2^(octs * repeat * contour * velocity accent)
//        -> x amp env^2 x velocity x pump(phase) -> soft clip (bounded +-1) -> Panner -> bus
// A legato note (landing on one still held) re-articulates: its gain restarts below full level and
// climbs over `attack`, so attack is audible on sequenced lines, not only on the first note.
//
// Pumper and Repeater are behaviours of the *beat grid*, not of the note: both read one phase
// derived from the engine transport (Instrument::syncTransport), so they keep ducking/opening on
// the beat through sustained notes, retriggers and silence, and several instances pump together.
// Pump sits at the gain stage (after filter and shaper) so the duck is clean and a resonant
// filter cannot smear it; it multiplies the ADSR rather than replacing it, so `pump = 0` is
// exactly the plain envelope. Repeat only opens the filter above its base cutoff (unipolar), so
// the fundamental never disappears.
//
// Reused DSP: AdsrEnvelope (amplitude; decay/sustain private at 0/1), BiquadFilter (24 dB LP),
// PercCore (lifecycle/pan/routing), rhythm.h (period, pump and repeat curves).
namespace webdsp {

inline constexpr InstrumentParamInfo kPulseParams[] = {
    {static_cast<int32_t>(PulseParam::Tune), "tune", 27.5f, 440.0f, 110.0f, "Hz"},
    {static_cast<int32_t>(PulseParam::Cutoff), "cutoff", 60.0f, 6000.0f, 500.0f, "Hz"},
    {static_cast<int32_t>(PulseParam::Resonance), "resonance", 0.0f, 1.0f, 0.25f, ""},
    {static_cast<int32_t>(PulseParam::Pump), "pump", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(PulseParam::Repeat), "repeat", 0.0f, 1.0f, 0.4f, ""},
    {static_cast<int32_t>(PulseParam::Rate), "rate", 0.0f, 5.0f, 2.0f, "step"},
    {static_cast<int32_t>(PulseParam::Diffuse), "diffuse", 0.0f, 1.0f, 0.3f, ""},
    {static_cast<int32_t>(PulseParam::Attack), "attack", 0.001f, 0.25f, 0.003f, "s"},
    {static_cast<int32_t>(PulseParam::Release), "release", 0.01f, 1.5f, 0.08f, "s"},
};
inline constexpr int32_t kPulseParamCount =
    static_cast<int32_t>(sizeof(kPulseParams) / sizeof(kPulseParams[0]));

class PulseInstrument final : public Instrument {
 public:
  PulseInstrument() {
    for (int32_t i = 0; i < kPulseParamCount; i++) values_[i] = kPulseParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Pulse; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    ampEnv_.configure(sampleRate);
    ampEnv_.reset();
    filter_ = BiquadFilter(sampleRate);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), 24.0f);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), value(PulseParam::Cutoff));
    dcBlockR_ = static_cast<float>(1.0 - kTwoPi * kDcCornerHz / sampleRate);
    slewCoef_ = static_cast<float>(std::exp(-1.0 / (kPumpSlewSeconds * sampleRate)));
    artSlewCoef_ = static_cast<float>(std::exp(-1.0 / (kArticulationSlewSeconds * sampleRate)));
    framesPerBeat_ = sampleRate * 0.5;
    qDirty_ = true;
    clearState();
  }

  void syncTransport(int64_t frame, double anchorFrame, double framesPerBeat) override {
    frame_ = static_cast<double>(frame);
    anchorFrame_ = anchorFrame;
    framesPerBeat_ = framesPerBeat;
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    // A note landing on one that is still held (legato, the tracker's default) would otherwise
    // find the envelope already at full level: attack would never be heard. Re-articulate it.
    const bool legato = core_.active && !core_.releasing;
    if (core_.begin(velocity, delayFrames, pan, pitch)) clearState();  // retrigger carries state
    else if (legato) artPending_ = true;
    ampEnv_.noteOn(ampParams());
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    ampEnv_.noteOff(ampParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kPulseParams, kPulseParamCount, values_, param, v);
    if (param == static_cast<int32_t>(PulseParam::Resonance)) qDirty_ = true;
  }

  void reset() override {
    ampEnv_.reset();
    clearState();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* mono = scratch[0];
    float* gains = scratch[1];
    const double sr = core_.sampleRate;

    const float cutoff = value(PulseParam::Cutoff);
    const float pumpAmount = value(PulseParam::Pump);
    const float repeat = value(PulseParam::Repeat);
    const float diffuse = value(PulseParam::Diffuse);
    const double tune = static_cast<double>(value(PulseParam::Tune)) * core_.pitchRatio;
    const float keytrack = static_cast<float>(std::sqrt(core_.pitchRatio));
    const float accent = 0.5f + 0.5f * std::min(core_.velocity, 1.0f);
    const float sideLevel = kSideLevel * diffuse;
    const float srcNorm = kSourcePeak / (1.0f + 2.0f * sideLevel + kSubLevel);
    const double detune = std::exp2(static_cast<double>(diffuse) * kMaxDetuneCents / 1200.0);
    const float levelGain = core_.velocity * kLevel;
    const double period =
        rhythm::periodFrames(framesPerBeat_, rhythm::rateStep(value(PulseParam::Rate)));
    const double phaseInc = 1.0 / std::max(period, 1.0);
    const float biasTanh = std::tanh(kBias);

    if (qDirty_) {
      const float r = value(PulseParam::Resonance);
      const float q = kQFlat + r * r * kQSpan;
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), q);
      qComp_ = 1.0f / (1.0f + kQComp * (q - kQFlat));
      qDirty_ = false;
    }

    // Pre-note delay: silence, and nothing advances. Sample i of this call sits at frame_ + i.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t k = 0; k < lead; k++) mono[k] = gains[k] = 0.0f;
    core_.pendingDelay -= lead;

    // Re-articulation of a legato note, from the frame the new note actually starts: the gain
    // restarts below full level and climbs over `attack`. How far it dips scales with attack
    // (a 3 ms default barely dips; 30 ms or more is a full re-attack), and a one-pole slew on
    // the result keeps even a full restart click-free.
    if (artPending_ && core_.pendingDelay == 0) {
      const float attack = value(PulseParam::Attack);
      artTarget_ = std::clamp(1.0f - attack / kArticulationFullAttack, 0.0f, 1.0f);
      artStep_ = (1.0f - artTarget_) / std::max(static_cast<float>(attack * sr), 1.0f);
      artPending_ = false;
    }

    const double f0 = std::clamp(tune, 20.0, std::min(1000.0, sr * 0.2));
    const double inc = f0 / sr;

    bool finished = false;
    int32_t i = lead;
    while (i < numFrames) {
      const int32_t n = std::min(kSubBlock, numFrames - i);
      if (ampEnv_.stage() == AdsrEnvelope::Stage::Idle) {
        finished = true;
        for (int32_t k = i; k < numFrames; k++) mono[k] = gains[k] = 0.0f;
        break;
      }

      // Grid phase at the start of this sub-block; the pump advances it per sample.
      double phase = rhythm::phaseAt(frame_ + i, anchorFrame_, period);

      // --- control rate: repeat contour (mid-sub-block) drives cutoff and drive.
      double midPhase = phase + 0.5 * n * phaseInc;
      midPhase -= std::floor(midPhase);
      const float contour = rhythm::repeatContour(static_cast<float>(midPhase)) * repeat;

      const float d = kDriveBase + kDriveSpan * contour * accent;
      const float driveDb = -20.0f + 50.0f * std::pow(d, 0.7f);
      const float g = std::pow(10.0f, driveDb * 0.05f);
      const float comp = kSourcePeak / std::max(std::tanh(g * kSourcePeak + kBias) - biasTanh, 0.02f);

      const float octs = kMaxRepeatOctaves * contour * accent;
      // Never closes below the fundamental (x0.9), as Bass: the filter can darken, not delete.
      const float floorHz = static_cast<float>(0.9 * f0);
      const float cut = std::clamp(std::max(cutoff * keytrack * std::exp2(octs), floorHz), 30.0f,
                                   static_cast<float>(std::min(18000.0, sr * 0.45)));
      if (std::fabs(cut - lastCutoff_) > 0.002f * cut) {
        filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), cut);
        lastCutoff_ = cut;
      }

      // --- audio rate: oscillators -> shaper -> DC block; amp x pump gain kept for after the filter.
      for (int32_t k = 0; k < n; k++) {
        const float centre = sawBlep(phaseC_, inc);
        const float up = sawBlep(phaseU_, inc * detune);
        const float down = sawBlep(phaseD_, inc / detune);
        advance(phaseC_, inc);
        advance(phaseU_, inc * detune);
        advance(phaseD_, inc / detune);
        advance(subPhase_, inc * 0.5);
        const float sub = static_cast<float>(std::sin(kTwoPi * subPhase_));
        const float src = srcNorm * (centre + sideLevel * (up + down) + kSubLevel * sub);

        const float shaped = (std::tanh(g * src + kBias) - biasTanh) * comp;
        float y = shaped - dcX1_ + dcBlockR_ * dcY1_;
        y = std::clamp(y, -2.0f, 2.0f);
        dcX1_ = shaped;
        dcY1_ = y;
        mono[i + k] = y;

        // Pump target from the grid; the one-pole slew is the hit's click-free ramp-in and also
        // smooths any rate/tempo change. At pump 0 the target is exactly 1.
        const float target = rhythm::pumpGain(static_cast<float>(phase), pumpAmount);
        pumpGain_ = target + slewCoef_ * (pumpGain_ - target);
        phase += phaseInc;
        phase -= std::floor(phase);

        if (artTarget_ < 1.0f) artTarget_ = std::min(1.0f, artTarget_ + artStep_);
        artGain_ = artTarget_ + artSlewCoef_ * (artGain_ - artTarget_);

        const float e = ampEnv_.next();
        gains[i + k] = e * e * artGain_ * artGain_ * levelGain * qComp_ * pumpGain_;
      }

      float* ch[1] = {mono + i};
      filter_.process(ch, 1, n);
      i += n;
    }

    for (int32_t k = lead; k < numFrames; k++) mono[k] = softClip(mono[k] * gains[k]);
    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr float kLevel = 0.8f;
  static constexpr float kSourcePeak = 0.7f;
  static constexpr int32_t kSubBlock = 16;  // control-rate step (~0.33 ms at 48 kHz)
  static constexpr double kTwoPi = 6.283185307179586;
  static constexpr double kDcCornerHz = 5.0;
  static constexpr double kPumpSlewSeconds = 0.0015;
  static constexpr double kArticulationSlewSeconds = 0.0007;
  static constexpr float kArticulationFullAttack = 0.03f;
  static constexpr float kQFlat = 0.7071f;
  static constexpr float kQSpan = 7.3f;  // resonance 1 -> Q ~8
  static constexpr float kQComp = 0.15f;
  static constexpr float kSubLevel = 0.35f;
  static constexpr float kSideLevel = 0.7f;
  static constexpr double kMaxDetuneCents = 20.0;
  static constexpr float kMaxRepeatOctaves = 3.0f;
  static constexpr float kBias = 0.35f;  // asymmetry: even harmonics
  static constexpr float kDriveBase = 0.25f;
  static constexpr float kDriveSpan = 0.45f;

  static int32_t idx(PulseParam p) { return static_cast<int32_t>(p); }
  float value(PulseParam p) const { return values_[idx(p)]; }

  // Applied squared like Bass (exponential-ish tail). Decay 0 / sustain 1 are private: the pump
  // supplies the dynamics, so the amp envelope is effectively attack/release.
  AdsrParams ampParams() const {
    return AdsrParams{value(PulseParam::Attack), 0.0f, 1.0f, value(PulseParam::Release)};
  }

  static void advance(double& phase, double inc) {
    phase += inc;
    phase -= std::floor(phase);
  }

  // Saw with its edge at phase 0, PolyBLEP-corrected.
  static float sawBlep(double phase, double inc) {
    float s = static_cast<float>(2.0 * phase - 1.0);
    if (phase < inc) {
      const double x = phase / inc;
      s -= static_cast<float>(x + x - x * x - 1.0);
    } else if (phase > 1.0 - inc) {
      const double x = (phase - 1.0) / inc;
      s -= static_cast<float>(x * x + x + x + 1.0);
    }
    return s;
  }

  void clearState() {
    // Staggered start phases keep the summed saws from peaking together on the first cycle.
    phaseC_ = 0.25;
    phaseU_ = 0.58;
    phaseD_ = 0.91;
    subPhase_ = 0.25;
    dcX1_ = dcY1_ = 0.0f;
    lastCutoff_ = -1.0f;
    pumpGain_ = 1.0f;
    artTarget_ = artGain_ = 1.0f;
    artStep_ = 0.0f;
    artPending_ = false;
    filter_.reset();
  }

  static float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.6f) return x;
    const float u = std::min((a - 0.6f) * 2.5f, 3.0f);
    const float y = 0.6f + 0.4f * (u * (27.0f + u * u) / (27.0f + 9.0f * u * u));
    const float c = std::min(y, 1.0f);
    return x < 0.0f ? -c : c;
  }

  float values_[kPulseParamCount] = {};
  PercCore core_;
  AdsrEnvelope ampEnv_;
  BiquadFilter filter_{48000.0};
  double phaseC_ = 0.25, phaseU_ = 0.58, phaseD_ = 0.91, subPhase_ = 0.25;
  float dcX1_ = 0.0f, dcY1_ = 0.0f, dcBlockR_ = 0.999f;
  float lastCutoff_ = -1.0f;
  float qComp_ = 1.0f;
  float pumpGain_ = 1.0f;
  float slewCoef_ = 0.98f;
  float artTarget_ = 1.0f, artGain_ = 1.0f, artStep_ = 0.0f, artSlewCoef_ = 0.97f;
  bool artPending_ = false;
  bool qDirty_ = true;
  // Transport (Instrument::syncTransport).
  double frame_ = 0.0;
  double anchorFrame_ = 0.0;
  double framesPerBeat_ = 24000.0;
};

}  // namespace webdsp
