#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/biquad_filter.h"
#include "../dsp/percussion.h"

// Synthesized "PWM Beast": a pulse-based character synth. Shipped behaviour, parameter mapping and
// the reasoning behind each stage: docs/pwm-beast-instrument.md.
//
//   PolyBLEP pulse (zero-mean, width w) + sub sine (-1 oct)
//        ^ w = width + amount * pwmSource      pwmSource = LFO (sine -> skewed ramp) -> timbre envelope
//        -> character:  edge rounding (1-pole, keytracked) -> asymmetric sine fold     } harmonics
//        -> drive:      asymmetric tanh, gain-compensated, rides the timbre envelope   } drive
//        -> DC block (~5 Hz)
//        -> BiquadFilter LP 24 dB, cutoff/Q/envelope                                   } cutoff, resonance, envamt
//        -> x amp env^2 x velocity -> soft clip (bounded +-1) -> Panner -> bus
//
// Why the character stage rounds first: a memoryless shaper on an ideal pulse does nothing
// interesting, because a pulse only ever takes two values and any curve maps two values to two
// values (a different pulse). Rounding the edges gives the waveform a shape *between* the levels,
// and the fold then reshapes it. Since the plateau levels of the rounded wave depend on the pulse
// width, PWM moves the harmonics through a nonlinear map, not just a comb filter.
//
// Reused DSP: AdsrEnvelope (x2: amplitude + internal timbre), BiquadFilter (24 dB LP), PercCore
// (lifecycle/pan/routing), setTableParam. PolyBLEP, tanh drive and soft clip are the same inline
// forms Bass/Pad use (Saturation is a DSPNode with a fixed DC blocker and a static drive; this
// needs a drive that moves during the note).
//
// Mono per instance; retrigger is continuous (phase, LFO and envelope levels carry); a fresh note
// resets phase/LFO/filter so identical inputs render identical samples. Not tempo-synced.
namespace webdsp {

inline constexpr InstrumentParamInfo kPwmBeastParams[] = {
    {static_cast<int32_t>(PwmBeastParam::Tune), "tune", 27.5f, 440.0f, 55.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(PwmBeastParam::Width), "width", 0.05f, 0.95f, 0.3f, "", ModDomain::Linear},
    {static_cast<int32_t>(PwmBeastParam::Pwm), "pwm", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(PwmBeastParam::Rate), "rate", 0.1f, 200.0f, 3.5f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(PwmBeastParam::Shape), "shape", 0.0f, 1.0f, 0.35f, "", ModDomain::Linear},
    {static_cast<int32_t>(PwmBeastParam::Harmonics), "harmonics", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(PwmBeastParam::Drive), "drive", 0.0f, 1.0f, 0.4f, "", ModDomain::Linear},
    {static_cast<int32_t>(PwmBeastParam::Cutoff), "cutoff", 60.0f, 8000.0f, 500.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(PwmBeastParam::Resonance), "resonance", 0.0f, 1.0f, 0.45f, "", ModDomain::Linear},
    {static_cast<int32_t>(PwmBeastParam::EnvAmount), "envamt", 0.0f, 1.0f, 0.7f, "", ModDomain::Linear},
    {static_cast<int32_t>(PwmBeastParam::Sub), "sub", 0.0f, 1.0f, 0.25f, "", ModDomain::Linear},
    {static_cast<int32_t>(PwmBeastParam::Attack), "attack", 0.001f, 0.25f, 0.003f, "s"},
    {static_cast<int32_t>(PwmBeastParam::Decay), "decay", 0.03f, 2.0f, 0.3f, "s"},
    {static_cast<int32_t>(PwmBeastParam::Sustain), "sustain", 0.0f, 1.0f, 0.6f, ""},
    {static_cast<int32_t>(PwmBeastParam::Release), "release", 0.01f, 1.5f, 0.12f, "s"},
};
inline constexpr int32_t kPwmBeastParamCount =
    static_cast<int32_t>(sizeof(kPwmBeastParams) / sizeof(kPwmBeastParams[0]));

class PwmBeastInstrument final : public Instrument {
 public:
  PwmBeastInstrument() {
    for (int32_t i = 0; i < kPwmBeastParamCount; i++) values_[i] = kPwmBeastParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::PwmBeast; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    ampEnv_.configure(sampleRate);
    timbreEnv_.configure(sampleRate);
    ampEnv_.reset();
    timbreEnv_.reset();
    filter_ = BiquadFilter(sampleRate);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), 24.0f);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), value(PwmBeastParam::Cutoff));
    dcBlockR_ = static_cast<float>(1.0 - kTwoPi * kDcCornerHz / sampleRate);
    lastCutoff_ = -1.0f;
    qDirty_ = true;
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    if (core_.begin(velocity, delayFrames, pan, pitch)) clearState();  // retrigger carries state
    ampEnv_.noteOnRestart(ampParams());
    timbreEnv_.noteOn(timbreParams());
  }

  void duck(int32_t frames) override {
    ampEnv_.duck(frames);
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    ampEnv_.noteOff(ampParams());
    timbreEnv_.noteOff(timbreParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kPwmBeastParams, kPwmBeastParamCount, values_, param, v);
    if (param == static_cast<int32_t>(PwmBeastParam::Resonance)) qDirty_ = true;
  }

  void reset() override {
    ampEnv_.reset();
    timbreEnv_.reset();
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

    const float width = value(PwmBeastParam::Width);
    const float pwm = value(PwmBeastParam::Pwm);
    const double rateInc = static_cast<double>(value(PwmBeastParam::Rate)) / sr;
    const float shape = value(PwmBeastParam::Shape);
    const float harmonics = value(PwmBeastParam::Harmonics);
    const float drive = value(PwmBeastParam::Drive);
    const float cutoff = value(PwmBeastParam::Cutoff);
    const float envAmt = value(PwmBeastParam::EnvAmount);
    const float sub = value(PwmBeastParam::Sub);
    const double f0Note = static_cast<double>(value(PwmBeastParam::Tune)) * core_.pitchRatio;
    const double f0 = std::clamp(f0Note, 20.0, std::min(kMaxF0, sr * 0.2));
    const double inc = f0 / sr;
    const float velocity = std::min(core_.velocity, 1.0f);
    const float vf = 0.6f + 0.4f * velocity;  // velocity opens the filter/drive a little, not only level
    const float levelGain = core_.velocity * kLevel;

    // Shape: 0..0.5 morphs the LFO sine -> skewed ramp; 0.5..1 fades the LFO into the timbre
    // envelope (pulse width sweeps with the note instead of cycling).
    const float rampMix = std::min(shape * 2.0f, 1.0f);
    const float envMix = std::max(shape * 2.0f - 1.0f, 0.0f);
    // Character stage. The edges are always rounded to a fixed multiple of the fundamental (so the
    // wave has curvature for the fold to work on, and keytracks across the keyboard); the dial sets
    // how hard it is folded and how much of the fold replaces the plain pulse. At high pitch the
    // fold is eased (it would alias) instead of exposing it.
    const float hEff = harmonics / (1.0f + static_cast<float>(f0 / kFoldEaseHz));
    const double fcRound = std::min(f0 * kRoundRatio, sr * 0.45);
    const float roundA = static_cast<float>(1.0 - std::exp(-kTwoPi * fcRound / sr));
    const float foldGain = kFoldBase + kFoldSpan * hEff;
    const float foldBias = kFoldBias * hEff;
    const float foldBiasSin = std::sin(kHalfPi * foldBias);
    const float foldMix = std::min(kFoldMixSlope * hEff, 1.0f);
    const float foldComp = 1.0f / (1.0f + kFoldCompSlope * hEff);

    if (qDirty_) {
      const float r = value(PwmBeastParam::Resonance);
      const float q = kQFlat + r * r * kQSpan;
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), q);
      qComp_ = 1.0f / (1.0f + kQComp * (q - kQFlat));
      qDirty_ = false;
    }

    // Pre-note delay: silence, and nothing advances.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t k = 0; k < lead; k++) mono[k] = gains[k] = 0.0f;
    core_.pendingDelay -= lead;

    // Width never gets closer to an edge than the pulse can be band-limited at this pitch.
    const float wMin = std::max(kMinWidth, static_cast<float>(2.5 * inc));
    const float wMax = 1.0f - wMin;

    bool finished = false;
    int32_t i = lead;
    while (i < numFrames) {
      const int32_t n = std::min(kSubBlock, numFrames - i);
      if (ampEnv_.stage() == AdsrEnvelope::Stage::Idle ||
          (ampEnv_.stage() == AdsrEnvelope::Stage::Sustain && ampEnv_.level() <= 0.0f)) {
        finished = true;
        for (int32_t k = i; k < numFrames; k++) mono[k] = gains[k] = 0.0f;
        break;
      }

      // --- control rate: drive and cutoff follow the timbre envelope.
      const float t = timbreEnv_.level();
      const float d = drive * (0.6f + 0.4f * t * vf);
      const float driveDb = kMaxDriveDb * std::pow(d, 0.75f);
      const float g = std::pow(10.0f, driveDb * 0.05f);
      const float bias = kDriveBias * d;
      const float biasTanh = std::tanh(bias);
      const float comp = kDrivePeak / std::max(std::tanh(g * kDrivePeak + bias) - biasTanh, 0.02f);

      const float octs = envAmt * kMaxEnvOctaves * t * vf;
      const float keytrack = static_cast<float>(std::exp2(std::log2(core_.pitchRatio) * 0.5));
      // Never closes below the note's own fundamental: dark and round, but not gone.
      const float floorHz = static_cast<float>(0.9 * f0);
      const float cut = std::clamp(std::max(cutoff * keytrack * std::exp2(octs), floorHz), 30.0f,
                                   static_cast<float>(std::min(18000.0, sr * 0.45)));
      if (std::fabs(cut - lastCutoff_) > 0.002f * cut) {
        filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), cut);
        lastCutoff_ = cut;
      }

      // --- audio rate.
      for (int32_t k = 0; k < n; k++) {
        const float te = timbreEnv_.level();

        // PWM source in [-1, 1].
        lfoPhase_ += rateInc;
        lfoPhase_ -= std::floor(lfoPhase_);
        const float sine = static_cast<float>(std::sin(kTwoPi * lfoPhase_));
        const float ramp = skewedRamp(static_cast<float>(lfoPhase_));
        const float lfo = sine + (ramp - sine) * rampMix;
        const float src = lfo + ((2.0f * te - 1.0f) - lfo) * envMix;
        const float w = std::clamp(width + pwm * kPwmDepth * src, wMin, wMax);

        phase_ += inc;
        phase_ -= std::floor(phase_);
        subPhase_ += inc * 0.5;
        subPhase_ -= std::floor(subPhase_);

        // pulse = saw(p - w) - saw(p): two PolyBLEP'd saws, so both edges are band-limited and the
        // mean is zero for any w (levels 1-w for a fraction w, -w for the rest).
        double q = phase_ - w;
        q -= std::floor(q);
        const float sawA = static_cast<float>(2.0 * q - 1.0) - polyBlep(q, inc);
        const float sawB = static_cast<float>(2.0 * phase_ - 1.0) - polyBlep(phase_, inc);
        // Narrow pulses carry less energy; level them toward equal rms (capped so spikes stay sane).
        const float pulseGain = std::min(0.5f / std::sqrt(w * (1.0f - w)), kMaxPulseGain);
        const float pulse = 0.5f * (sawA - sawB) * pulseGain;
        const float subSine = static_cast<float>(std::sin(kTwoPi * subPhase_));
        const float osc = (pulse + kSubLevel * sub * subSine) / (1.0f + 0.4f * sub);

        // Character: round the edges, then fold.
        round_ += roundA * (osc - round_);
        const float u = foldGain * round_ + foldBias;
        const float folded = (std::sin(kHalfPi * u) - foldBiasSin) * foldComp;
        // Blended over the unrounded pulse, not in place of it: the fold adds harmonics to the
        // pulse body, so it can colour the note but never cancel the fundamental away.
        const float character = osc + (folded - osc) * foldMix;

        // Drive, then DC block.
        const float shaped = (std::tanh(g * character + bias) - biasTanh) * comp;
        float y = shaped - dcX1_ + dcBlockR_ * dcY1_;
        y = std::clamp(y, -2.0f, 2.0f);
        dcX1_ = shaped;
        dcY1_ = y;
        mono[i + k] = y;

        const float e = ampEnv_.next();
        timbreEnv_.next();
        gains[i + k] = e * e * levelGain * qComp_;
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
  static constexpr float kLevel = 0.64f;
  static constexpr int32_t kSubBlock = 16;        // control-rate step (~0.33 ms at 48 kHz)
  static constexpr double kTwoPi = 6.283185307179586;
  static constexpr float kHalfPi = 1.5707963267948966f;
  static constexpr double kDcCornerHz = 5.0;
  static constexpr double kMaxF0 = 3000.0;
  static constexpr float kMinWidth = 0.03f;
  static constexpr float kPwmDepth = 0.42f;       // pwm 1 swings the width +-0.42
  static constexpr float kMaxPulseGain = 1.6f;
  static constexpr float kSubLevel = 0.7f;
  // Character stage: edges rounded at 2 x f0, then a sine fold of rising gain and asymmetry.
  static constexpr double kRoundRatio = 2.0;
  static constexpr float kFoldBase = 1.0f;
  static constexpr float kFoldSpan = 4.0f;
  static constexpr float kFoldBias = 0.3f;
  static constexpr float kFoldMixSlope = 1.6f;
  static constexpr float kFoldCompSlope = 0.15f;
  static constexpr float kFoldEaseHz = 1500.0f;
  // Drive stage: asymmetric tanh, up to +34 dB, compensated at the reference peak.
  static constexpr float kMaxDriveDb = 34.0f;
  static constexpr float kDriveBias = 0.45f;
  static constexpr float kDrivePeak = 0.6f;
  static constexpr float kQFlat = 0.7071f;
  static constexpr float kQSpan = 11.0f;          // resonance 1 -> Q ~11.7
  static constexpr float kQComp = 0.12f;
  static constexpr float kMaxEnvOctaves = 5.0f;

  static int32_t idx(PwmBeastParam p) { return static_cast<int32_t>(p); }
  float value(PwmBeastParam p) const { return values_[idx(p)]; }

  // Amplitude envelope; applied squared, so sustain is fed as its square root (as Bass).
  AdsrParams ampParams() const {
    return AdsrParams{value(PwmBeastParam::Attack), value(PwmBeastParam::Decay),
                      std::sqrt(value(PwmBeastParam::Sustain)), value(PwmBeastParam::Release)};
  }
  // Internal timbre envelope (filter, drive, PWM sweep): closes faster and rests lower than the
  // level, so the note opens bright and settles into the sustained PWM tone.
  AdsrParams timbreParams() const {
    return AdsrParams{value(PwmBeastParam::Attack), value(PwmBeastParam::Decay) * 0.6f,
                      value(PwmBeastParam::Sustain) * 0.3f, value(PwmBeastParam::Release)};
  }

  // Ramp that rises over 90% of the cycle and falls over the last 10%: the character of a saw
  // without a step in the pulse width.
  static float skewedRamp(float p) {
    return p < 0.9f ? -1.0f + 2.0f * p / 0.9f : 1.0f - 2.0f * (p - 0.9f) / 0.1f;
  }

  // Edge residual for a unit step at phase 0, dt = phase increment (as Bass/Pad).
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
    phase_ = 0.25;
    subPhase_ = 0.25;
    lfoPhase_ = 0.0;
    round_ = 0.0f;
    dcX1_ = dcY1_ = 0.0f;
    lastCutoff_ = -1.0f;
    filter_.reset();
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

  float values_[kPwmBeastParamCount] = {};
  PercCore core_;
  AdsrEnvelope ampEnv_, timbreEnv_;
  BiquadFilter filter_{48000.0};
  double phase_ = 0.25, subPhase_ = 0.25, lfoPhase_ = 0.0;
  float round_ = 0.0f;
  float dcX1_ = 0.0f, dcY1_ = 0.0f, dcBlockR_ = 0.999f;
  float lastCutoff_ = -1.0f;
  float qComp_ = 1.0f;
  bool qDirty_ = true;
};

}  // namespace webdsp
