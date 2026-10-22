#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/biquad_filter.h"
#include "../dsp/fm_kernel.h"
#include "../dsp/percussion.h"

// Synthesized "Percussion Synth": a small two-oscillator + noise synthesizer that is unusually good
// at percussion. Behaviour, parameter mapping and sound-design recipes: docs/percussion-synth.md.
//
//   Osc 1 --------------------------------+--> x level1 --+
//     |  (raw output, before its level)   |                |
//     +--FM (mean pitch kept)--> Osc 2 --> x level2 --+          +--> mix --> BiquadFilter (LP/HP 12 dB)
//   Noise (colour, own decay) --> x level -----+                          ^ cutoff + filter envelope
//        --> drive (tanh + soft feedback, DC block) --> x amp env^2 x velocity --> soft clip --> bus
//
//   pitch envelope: one exponential sweep, osc 1 by `pitchenv` octaves, osc 2 by `osc2env` x that
//
// An original design in the spirit of analogue percussion synths; not a model of any of them.
//
// Reused DSP: AdsrEnvelope (amplitude), PercEnv (pitch, noise and filter envelopes), PercNoise,
// BiquadFilter, PercCore (lifecycle/pan/routing), fmk sine table, setTableParam. PolyBLEP, tanh
// drive and soft clip are the same inline forms Bass/Pad/PWM Beast use.
//
// Mono per instance; retrigger is continuous (oscillator phase and filter carry; the sweeps restart and the
// amplitude envelope fades out over ~2 ms, then re-attacks from silence); a fresh note resets everything so identical inputs render identical samples.
// Not tempo-synced. No allocation after configure().
namespace webdsp {

inline constexpr InstrumentParamInfo kPercSynthParams[] = {
    {static_cast<int32_t>(PercSynthParam::Tune), "tune", 20.0f, 2000.0f, 55.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(PercSynthParam::Osc1Wave), "osc1wave", 0.0f, 2.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(PercSynthParam::Osc1Level), "osc1level", 0.0f, 1.0f, 0.9f, "", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::Osc2Wave), "osc2wave", 0.0f, 2.0f, 1.0f, "step", ModDomain::None},
    {static_cast<int32_t>(PercSynthParam::Osc2Pitch), "osc2pitch", -24.0f, 48.0f, 12.0f, "st", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::Osc2Level), "osc2level", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::Osc2Fm), "osc2fm", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::PitchEnv), "pitchenv", 0.0f, 6.0f, 2.5f, "oct", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::PitchDecay), "pitchdecay", 0.005f, 2.0f, 0.07f, "s", ModDomain::Log},
    {static_cast<int32_t>(PercSynthParam::Osc2Env), "osc2env", 0.0f, 2.0f, 1.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::Noise), "noise", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::NoiseTone), "noisetone", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::NoiseDecay), "noisedecay", 0.003f, 2.0f, 0.04f, "s", ModDomain::Log},
    {static_cast<int32_t>(PercSynthParam::FilterType), "filtertype", 0.0f, 1.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(PercSynthParam::Cutoff), "cutoff", 40.0f, 16000.0f, 5000.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(PercSynthParam::Resonance), "resonance", 0.0f, 1.0f, 0.15f, "", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::FilterEnv), "filterenv", -1.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(PercSynthParam::FilterDecay), "filterdecay", 0.01f, 2.0f, 0.15f, "s", ModDomain::Log},
    {static_cast<int32_t>(PercSynthParam::Attack), "attack", 0.0005f, 0.25f, 0.001f, "s"},
    {static_cast<int32_t>(PercSynthParam::Decay), "decay", 0.02f, 4.0f, 0.35f, "s"},
    {static_cast<int32_t>(PercSynthParam::Sustain), "sustain", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(PercSynthParam::Release), "release", 0.005f, 4.0f, 0.12f, "s"},
    {static_cast<int32_t>(PercSynthParam::Drive), "drive", 0.0f, 1.0f, 0.2f, "", ModDomain::Linear},
};
inline constexpr int32_t kPercSynthParamCount =
    static_cast<int32_t>(sizeof(kPercSynthParams) / sizeof(kPercSynthParams[0]));

class PercSynthInstrument final : public Instrument {
 public:
  PercSynthInstrument() {
    for (int32_t i = 0; i < kPercSynthParamCount; i++) values_[i] = kPercSynthParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::PercSynth; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    ampEnv_.configure(sampleRate);
    ampEnv_.reset();
    pitchEnv_.configure(sampleRate);
    noiseEnv_.configure(sampleRate);
    filterEnv_.configure(sampleRate);
    sine_ = fmk::sineTable();
    filter_ = BiquadFilter(sampleRate);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), 12.0f);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), value(PercSynthParam::Cutoff));
    dcBlockR_ = static_cast<float>(1.0 - kTwoPi * kDcCornerHz / sampleRate);
    fbA_ = static_cast<float>(1.0 - std::exp(-kTwoPi * kFeedbackHz / sampleRate));
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    if (core_.begin(velocity, delayFrames, pan, pitch)) clearState();  // retrigger carries state
    ampEnv_.noteOnRestart(ampParams());
    // The sweeps always restart from full depth (the first sample of a hit is the highest pitch).
    pitchEnv_.trigger(percDecayCoef(value(PercSynthParam::PitchDecay) * kDecayToTau, core_.sampleRate), kInstantAttack);
    noiseEnv_.trigger(percDecayCoef(value(PercSynthParam::NoiseDecay) * kDecayToTau, core_.sampleRate), kInstantAttack);
    filterEnv_.trigger(percDecayCoef(value(PercSynthParam::FilterDecay) * kDecayToTau, core_.sampleRate), kInstantAttack);
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    ampEnv_.noteOff(ampParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kPercSynthParams, kPercSynthParamCount, values_, param, v);
    if (param == static_cast<int32_t>(PercSynthParam::Resonance)) qDirty_ = true;
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

    const int32_t wave1 = waveIndex(value(PercSynthParam::Osc1Wave));
    const int32_t wave2 = waveIndex(value(PercSynthParam::Osc2Wave));
    const float velocity = std::min(core_.velocity, 1.0f);
    // Velocity is mostly level, plus a little character: more FM, more noise, a brighter filter
    // sweep and a harder drive on hard hits. Each is a few tenths at most, and monotonic.
    const float vFm = 0.7f + 0.3f * velocity;
    const float vNoise = 0.7f + 0.3f * velocity;
    const float vFilter = 0.7f + 0.3f * velocity;
    const float vDrive = 0.6f + 0.4f * velocity;

    const double f1 = static_cast<double>(value(PercSynthParam::Tune)) * core_.pitchRatio;
    const double f2 = f1 * std::exp2(static_cast<double>(value(PercSynthParam::Osc2Pitch)) / 12.0);
    const double incMax = 0.45;  // instantaneous frequency stays below 0.45 fs: PolyBLEP stays valid
    const double inc1Rest = f1 / sr, inc2Rest = f2 / sr;
    const float sweep1 = value(PercSynthParam::PitchEnv);
    const float sweep2 = sweep1 * value(PercSynthParam::Osc2Env);
    const float fmDepth = kMaxFmDepth * sqr(value(PercSynthParam::Osc2Fm)) * vFm;
    const float level1 = value(PercSynthParam::Osc1Level);
    const float level2 = value(PercSynthParam::Osc2Level);
    const float noiseLevel = value(PercSynthParam::Noise) * kNoiseLevel * vNoise;
    const float mixComp = 1.0f / (0.6f + 0.4f * (level1 + level2 + value(PercSynthParam::Noise)));

    // Noise colour: below 0.5 a low-pass sweeping down from open, above it white minus a low-pass
    // sweeping up from 20 Hz (a high-pass). Both meet at "white" in the middle.
    const float tone = value(PercSynthParam::NoiseTone);
    const bool noiseDark = tone < 0.5f;
    const double noiseHz = noiseDark ? 18000.0 * std::exp2(-(0.5 - tone) * 2.0 * 7.0)
                                     : 20.0 * std::exp2((tone - 0.5) * 2.0 * 9.5);
    const float noiseA = static_cast<float>(1.0 - std::exp(-kTwoPi * std::min(noiseHz, sr * 0.45) / sr));
    // Level back what the one pole took away (dark noise keeps a narrow band, bright a thin top).
    const float u = (tone - 0.5f) * 2.0f;
    const float noiseComp = noiseDark
        ? std::clamp(static_cast<float>(std::sqrt(15300.0 / noiseHz)), 1.0f, 5.0f)
        : 1.0f + 2.4f * u * u * u;

    const float cutoff = value(PercSynthParam::Cutoff);
    const float envAmt = value(PercSynthParam::FilterEnv);
    const bool highPass = value(PercSynthParam::FilterType) >= 0.5f;
    const float keytrack = static_cast<float>(std::sqrt(core_.pitchRatio));
    if (highPass != lastHighPass_) {
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterMode), highPass ? 1.0f : 0.0f);
      lastHighPass_ = highPass;
      lastCutoff_ = -1.0f;
    }
    if (qDirty_) {
      const float r = value(PercSynthParam::Resonance);
      const float q = kQFlat + r * r * kQSpan;
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), q);
      qComp_ = 1.0f / (1.0f + kQComp * (q - kQFlat));
      qDirty_ = false;
    }

    const float levelGain = core_.velocity * kLevel;

    // Pre-note delay: silence, and nothing advances.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t k = 0; k < lead; k++) mono[k] = gains[k] = 0.0f;
    core_.pendingDelay -= lead;

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

      // --- control rate: the filter follows its envelope.
      const float octs = envAmt * kMaxEnvOctaves * filterEnv_.level() * vFilter;
      const float cut = std::clamp(cutoff * keytrack * std::exp2(octs), 30.0f,
                                   static_cast<float>(std::min(18000.0, sr * 0.45)));
      if (std::fabs(cut - lastCutoff_) > 0.002f * cut) {
        filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), cut);
        lastCutoff_ = cut;
      }

      // --- audio rate.
      for (int32_t k = 0; k < n; k++) {
        const float sw = pitchEnv_.next();  // 1 on the first sample of a hit, falling to 0

        // Oscillator 1 (also the FM source: its raw output, whatever its level).
        const double inc1 = std::min(inc1Rest * std::exp2(static_cast<double>(sweep1 * sw)), incMax);
        phase1_ += inc1;
        phase1_ -= std::floor(phase1_);
        const float o1 = oscillator(wave1, phase1_, inc1);

        // Oscillator 2, its frequency pushed around by osc 1: f2 * (1 + depth * o1). Linear in
        // frequency, so the carrier stays tuned (osc 1 is zero-mean) and the index follows the
        // f2 : f1 ratio. |o1| <= 1 and the multiplier is floored, so the increment stays positive
        // (no through-zero flip, which PolyBLEP could not follow) and below 0.45.
        double inc2 = inc2Rest * std::exp2(static_cast<double>(sweep2 * sw));
        inc2 *= std::max(1.0f + fmDepth * o1, kMinFmRatio);
        inc2 = std::min(inc2, incMax);
        phase2_ += inc2;
        phase2_ -= std::floor(phase2_);
        const float o2 = oscillator(wave2, phase2_, inc2);

        // Noise, coloured by one pole, with its own decay.
        const float white = noise_.next();
        noiseLp_ += noiseA * (white - noiseLp_);
        const float coloured = (noiseDark ? noiseLp_ : white - noiseLp_) * noiseComp;
        const float nz = coloured * noiseEnv_.next() * noiseLevel;

        mono[i + k] = (o1 * level1 + o2 * level2 + nz) * mixComp;
        filterEnv_.next();
        const float e = ampEnv_.next();
        gains[i + k] = e * e * levelGain * qComp_;
      }

      float* ch[1] = {mono + i};
      filter_.process(ch, 1, n);
      i += n;
    }

    // Character stage, after the filter so a resonant ring is saturated too, then the VCA.
    const float drive = value(PercSynthParam::Drive) * vDrive;
    const float driveDb = kMaxDriveDb * std::pow(drive, 0.8f);
    const float g = std::pow(10.0f, driveDb * 0.05f);
    // The offset is in signal units (before the gain), so its effect on the duty cycle of a hard
    // driven wave does not vanish as the gain rises: the asymmetry adds even harmonics.
    const float bias = kDriveBias * drive;
    const float biasTanh = std::tanh(g * bias);
    const float comp = kDrivePeak / std::max(0.5f * (std::tanh(g * (kDrivePeak + bias)) -
                                                     std::tanh(g * (bias - kDrivePeak))), 0.02f);
    const float wet = std::min(drive * 6.0f, 1.0f);       // exactly transparent at drive 0
    const float fbk = kMaxFeedback * sqr(drive);          // < 1: the loop cannot latch
    for (int32_t k = lead; k < numFrames; k++) {
      const float x = mono[k];
      float shaped = x;
      if (wet > 0.0f) {
        // The previous output (low-passed) leans on the input: a bounded ring that thickens lows.
        // tanh bounds `fbState_` to +-1 and fbk < 1, so the loop's small-signal gain is below 1.
        const float y = (std::tanh(g * (x + bias) + fbk * fbState_) - biasTanh) * comp;
        fbState_ += fbA_ * (std::tanh(y) - fbState_);
        if (std::fabs(fbState_) < 1e-20f) fbState_ = 0.0f;
        shaped = x + (y - x) * wet;
      }
      float y = shaped - dcX1_ + dcBlockR_ * dcY1_;
      y = std::clamp(y, -4.0f, 4.0f);
      dcX1_ = shaped;
      dcY1_ = std::fabs(y) < 1e-20f ? 0.0f : y;
      mono[k] = softClip(y * gains[k]);
    }

    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr float kLevel = 0.7f;
  static constexpr int32_t kSubBlock = 16;        // control-rate step (~0.33 ms at 48 kHz)
  static constexpr double kTwoPi = 6.283185307179586;
  static constexpr double kDcCornerHz = 5.0;
  static constexpr float kInstantAttack = 1e-6f;  // PercEnv clamps to one frame
  static constexpr float kNoiseLevel = 0.8f;
  static constexpr float kMaxFmDepth = 3.0f;      // osc2fm 1: osc 2's frequency swings by +-300 % of itself
  static constexpr float kMinFmRatio = 0.05f;
  static constexpr float kMaxEnvOctaves = 6.0f;
  static constexpr float kQFlat = 0.7071f;
  static constexpr float kQSpan = 13.0f;          // resonance 1 -> Q ~13.7
  static constexpr float kQComp = 0.12f;
  // Character stage: asymmetric tanh up to +24 dB, compensated at the reference peak, with a
  // low-passed one-sample feedback (<= 0.6 of loop gain) for body.
  static constexpr float kMaxDriveDb = 24.0f;
  static constexpr float kDriveBias = 0.15f;
  static constexpr float kDrivePeak = 0.6f;
  static constexpr float kMaxFeedback = 0.6f;
  static constexpr double kFeedbackHz = 900.0;

  static int32_t idx(PercSynthParam p) { return static_cast<int32_t>(p); }
  float value(PercSynthParam p) const { return values_[idx(p)]; }
  static float sqr(float x) { return x * x; }
  static int32_t waveIndex(float v) { return std::clamp(static_cast<int32_t>(v + 0.5f), 0, 2); }

  // Amplitude envelope; applied squared, so sustain is fed as its square root (as Bass/PWM Beast).
  AdsrParams ampParams() const {
    return AdsrParams{value(PercSynthParam::Attack), value(PercSynthParam::Decay),
                      std::sqrt(value(PercSynthParam::Sustain)), value(PercSynthParam::Release)};
  }

  // One cycle at phase p (0..1) with per-sample increment dt. All three start near zero so a
  // fresh hit does not open with a step: sine/triangle and saw start at 0, the square is the only
  // wave with a level at phase 0 (the amplitude attack covers it).
  float oscillator(int32_t wave, double p, double dt) const {
    switch (wave) {
      case 0: {  // half sine, half triangle: round, but not a pure sine
        const double q = p + 0.25 - std::floor(p + 0.25);
        const float tri = static_cast<float>(1.0 - 4.0 * std::fabs(q - 0.5));
        return 0.5f * (fmk::sinTurns(sine_, p) + tri);
      }
      case 1: {  // saw, rising from 0 at phase 0
        const double q = p + 0.5 - std::floor(p + 0.5);
        return static_cast<float>(2.0 * q - 1.0) - polyBlep(q, dt);
      }
      default: {  // square
        const double h = p + 0.5 - std::floor(p + 0.5);
        return (p < 0.5 ? 1.0f : -1.0f) + polyBlep(p, dt) - polyBlep(h, dt);
      }
    }
  }

  // Edge residual for a unit step at phase 0, dt = phase increment (as Bass/Pad/PWM Beast).
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
    phase1_ = phase2_ = 0.0;
    noise_.reset();
    noiseLp_ = 0.0f;
    pitchEnv_.reset();
    noiseEnv_.reset();
    filterEnv_.reset();
    fbState_ = 0.0f;
    dcX1_ = dcY1_ = 0.0f;
    lastCutoff_ = -1.0f;
    lastHighPass_ = false;
    qDirty_ = true;
    filter_.reset();
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterMode), 0.0f);
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

  float values_[kPercSynthParamCount] = {};
  PercCore core_;
  AdsrEnvelope ampEnv_;
  PercEnv pitchEnv_, noiseEnv_, filterEnv_;
  PercNoise noise_;
  BiquadFilter filter_{48000.0};
  const float* sine_ = nullptr;
  double phase1_ = 0.0, phase2_ = 0.0;
  float noiseLp_ = 0.0f;
  float fbState_ = 0.0f, fbA_ = 0.1f;
  float dcX1_ = 0.0f, dcY1_ = 0.0f, dcBlockR_ = 0.999f;
  float lastCutoff_ = -1.0f;
  float qComp_ = 1.0f;
  bool qDirty_ = true;
  bool lastHighPass_ = false;
};

}  // namespace webdsp
