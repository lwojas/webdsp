#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/biquad_filter.h"
#include "../dsp/percussion.h"

// Synthesized bass: "Shaped Core". Design rationale: docs/bass-instrument-proposal.md; shipped
// behaviour and parameter mapping: docs/bass-instrument.md.
//
//   saw (PolyBLEP) --+
//   sine ------------+-- body crossfade -- + sub sine (-1 oct)
//                                |
//        asymmetric tanh  <- drive = f(bite, timbre env, velocity)      (dynamic, per sub-block)
//                                | DC block (~5 Hz, bass-safe)
//   BiquadFilter LP 24 dB <- cutoff = f(cutoff, keytrack, timbre env, velocity) >= 0.9 f0, Q = f(resonance)
//                                |
//        x amp env^2 x velocity -> soft clip (bounded +-1) -> Panner -> bus
//
// What makes it a bass rather than "oscillator -> filter": the sine/sub keep the fundamental when
// the filter closes; the shaper sits *before* the filter so the moving low-pass windows harmonics
// the note generates; drive and cutoff follow one internal timbre envelope (decays faster than
// level), so the note starts rich and settles round; velocity changes timbre, not only level.
//
// Reused DSP: AdsrEnvelope (x2: amplitude + internal timbre), BiquadFilter (24 dB LP), PercCore
// (lifecycle/pan/routing). The shaper is inline rather than `Saturation`: it needs a drive that
// moves during the note, and Saturation's fixed 0.995 DC blocker has a ~38 Hz corner at 48 kHz,
// which would eat a bass fundamental.
//
// Mono per instance; retrigger is continuous (phase and envelope levels carry); a fresh note
// resets phase/filter so identical inputs render identical samples.
//
// Legato (param `legato` on): a note landing on a note that is still *held* (sounding, not
// releasing) continues the same voice. Envelopes, velocity and timbre state are untouched (no
// re-attack, no thump, no level step); only the pitch changes, jumping or, with `slide` > 0,
// gliding there. Ownership is last-note priority: `noteOff()` carries no note identity and the
// engine arms one release per instrument, so there is no held-note stack; the sequencer's
// per-note `duration` is what hands the voice from one note to the next. A note on a releasing
// voice is an ordinary retrigger (it re-attacks), and never glides.
namespace webdsp {

inline constexpr InstrumentParamInfo kBassParams[] = {
    {static_cast<int32_t>(BassParam::Tune), "tune", 27.5f, 110.0f, 55.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(BassParam::Body), "body", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(BassParam::Bite), "bite", 0.0f, 1.0f, 0.4f, "", ModDomain::Linear},
    {static_cast<int32_t>(BassParam::Cutoff), "cutoff", 40.0f, 4000.0f, 400.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(BassParam::Resonance), "resonance", 0.0f, 1.0f, 0.25f, "", ModDomain::Linear},
    {static_cast<int32_t>(BassParam::Attack), "attack", 0.001f, 0.25f, 0.003f, "s"},
    {static_cast<int32_t>(BassParam::Decay), "decay", 0.03f, 2.0f, 0.35f, "s"},
    {static_cast<int32_t>(BassParam::Sustain), "sustain", 0.0f, 1.0f, 0.6f, ""},
    {static_cast<int32_t>(BassParam::Release), "release", 0.01f, 1.5f, 0.1f, "s"},
    {static_cast<int32_t>(BassParam::Legato), "legato", 0.0f, 1.0f, 0.0f, "", ModDomain::None},
    {static_cast<int32_t>(BassParam::Slide), "slide", 0.0f, 0.5f, 0.0f, "s"},
};
inline constexpr int32_t kBassParamCount =
    static_cast<int32_t>(sizeof(kBassParams) / sizeof(kBassParams[0]));

class BassInstrument final : public Instrument {
 public:
  BassInstrument() {
    for (int32_t i = 0; i < kBassParamCount; i++) values_[i] = kBassParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Bass; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    ampEnv_.configure(sampleRate);
    timbreEnv_.configure(sampleRate);
    ampEnv_.reset();
    timbreEnv_.reset();
    filter_ = BiquadFilter(sampleRate);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), 24.0f);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), values_[idx(BassParam::Cutoff)]);
    dcBlockR_ = static_cast<float>(1.0 - 6.283185307179586 * kDcCornerHz / sampleRate);
    lastCutoff_ = -1.0f;
    qDirty_ = true;
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    // Only a note that is already audible and held can be legato (a pending-delay note has not
    // started, and a delayed one would zero-fill the held note).
    const bool tied = tied_;
    tied_ = false;  // a tie applies to one note
    const bool legato = (tied || value(BassParam::Legato) >= 0.5f) && core_.active && !core_.releasing &&
                        core_.pendingDelay == 0 && delayFrames <= 0;
    if (legato) {
      const float heldVelocity = core_.velocity;
      core_.begin(velocity, 0, pan, pitch);
      core_.velocity = heldVelocity;  // a velocity step would be an audible level jump
      startGlide(semitonesOf(core_.pitchRatio));
      return;
    }
    if (core_.begin(velocity, delayFrames, pan, pitch)) {
      // Fresh note. Start at the sine's peak so the first cycle punches immediately; the >= 1 ms
      // attack ramp (not the phase) is what keeps it click-free. A retrigger carries all state.
      clearState();
    }
    pitchSemis_ = targetSemis_ = semitonesOf(core_.pitchRatio);
    glideStep_ = 0.0;
    thump_ = 1.0f;
    ampEnv_.noteOnRestart(ampParams());
    timbreEnv_.noteOn(timbreParams());
  }

  void duck(int32_t frames) override {
    if (value(BassParam::Legato) >= 0.5f && core_.active && !core_.releasing) return;  // legato glides, it does not re-attack
    ampEnv_.duck(frames);
  }

  void setTie(bool tied) override {
    tied_ = tied;
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    ampEnv_.noteOff(ampParams());
    timbreEnv_.noteOff(timbreParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kBassParams, kBassParamCount, values_, param, v);
    if (param == static_cast<int32_t>(BassParam::Resonance)) qDirty_ = true;
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

    const float body = value(BassParam::Body);
    const float bite = value(BassParam::Bite);
    const float cutoff = value(BassParam::Cutoff);
    const double tuneHz = static_cast<double>(value(BassParam::Tune));
    // Velocity only changes timbre in proportion to bite; at bite 0 it is a pure level.
    const float vf = 1.0f - bite * (1.0f - std::min(core_.velocity, 1.0f));
    const float subLevel = 0.35f + 0.15f * (1.0f - body);
    const float srcNorm = kSourcePeak / (1.0f + subLevel);
    const float thumpCoef = percDecayCoef(kThumpSeconds, sr);
    const float levelGain = core_.velocity * kLevel;

    if (qDirty_) {
      const float r = value(BassParam::Resonance);
      const float q = kQFlat + r * r * kQSpan;
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), q);
      qComp_ = 1.0f / (1.0f + kQComp * (q - kQFlat));
      qDirty_ = false;
    }

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

      // --- control rate: one timbre sample per sub-block drives drive, cutoff and pitch thump.
      const float t = timbreEnv_.level();
      const float d = bite * (0.35f + 0.65f * t * vf);
      const float driveDb = -20.0f + 50.0f * std::pow(d, 0.7f);
      const float g = std::pow(10.0f, driveDb * 0.05f);
      const float bias = 0.2f + 0.6f * bite;  // asymmetry: even harmonics (the 2nd carries on small speakers)
      const float biasTanh = std::tanh(bias);
      const float comp = kSourcePeak / std::max(std::tanh(g * kSourcePeak + bias) - biasTanh, 0.02f);

      // Glide: semitone position moves a fixed amount per sample (block-size independent) and
      // lands exactly on the target. Evaluated per sub-block, like everything at control rate.
      if (pitchSemis_ != targetSemis_) {
        pitchSemis_ += glideStep_ * n;
        if (glideStep_ > 0.0 ? pitchSemis_ >= targetSemis_ : pitchSemis_ <= targetSemis_) {
          pitchSemis_ = targetSemis_;
        }
      }
      const double tune = tuneHz * std::exp2(pitchSemis_ / 12.0);
      const float keytrack = static_cast<float>(std::exp2(pitchSemis_ / 24.0));

      const float thumpSemis = kThumpSemitones * bite * vf * thump_;
      const double f0 = std::clamp(tune * std::exp2(static_cast<double>(thumpSemis) / 12.0), 20.0,
                                   std::min(1000.0, sr * 0.2));
      const double inc = f0 / sr;
      thump_ *= std::pow(thumpCoef, static_cast<float>(n));

      const float octs = bite * kMaxEnvOctaves * t * (0.4f + 0.6f * vf);
      // The cutoff never closes below the note's own fundamental (x0.9): the filter can make the
      // note dark and round, but it cannot make it disappear. This is what keeps the weight.
      const float floorHz = static_cast<float>(0.9 * f0);
      const float cut = std::clamp(std::max(cutoff * keytrack * std::exp2(octs), floorHz), 30.0f,
                                   static_cast<float>(std::min(18000.0, sr * 0.45)));
      if (std::fabs(cut - lastCutoff_) > 0.002f * cut) {
        filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), cut);
        lastCutoff_ = cut;
      }

      // --- audio rate: oscillators -> shaper -> DC block; amp gain stored for after the filter.
      for (int32_t k = 0; k < n; k++) {
        phase_ += inc;
        phase_ -= std::floor(phase_);
        subPhase_ += inc * 0.5;
        subPhase_ -= std::floor(subPhase_);

        // Saw with its edge at phase 0.5 so it shares the sine's zero crossing at phase 0.
        double sawPhase = phase_ + 0.5;
        sawPhase -= std::floor(sawPhase);
        float saw = static_cast<float>(2.0 * sawPhase - 1.0);
        if (sawPhase < inc) {
          const double x = sawPhase / inc;
          saw -= static_cast<float>(x + x - x * x - 1.0);
        } else if (sawPhase > 1.0 - inc) {
          const double x = (sawPhase - 1.0) / inc;
          saw -= static_cast<float>(x * x + x + x + 1.0);
        }
        const float sine = static_cast<float>(std::sin(kTwoPi * phase_));
        const float sub = static_cast<float>(std::sin(kTwoPi * subPhase_));
        const float src = srcNorm * (body * saw + (1.0f - body) * sine + subLevel * sub);

        const float shaped = (std::tanh(g * src + bias) - biasTanh) * comp;
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
  static constexpr float kLevel = 0.8f;
  static constexpr float kSourcePeak = 0.7f;      // nominal shaper input peak (comp reference)
  static constexpr int32_t kSubBlock = 16;        // control-rate step (~0.33 ms at 48 kHz)
  static constexpr double kTwoPi = 6.283185307179586;
  static constexpr double kDcCornerHz = 5.0;
  static constexpr float kQFlat = 0.7071f;
  static constexpr float kQSpan = 9.0f;           // resonance 1 -> Q ~9.7
  static constexpr float kQComp = 0.15f;
  static constexpr float kMaxEnvOctaves = 4.0f;
  static constexpr float kThumpSemitones = 0.5f;
  static constexpr float kThumpSeconds = 0.012f;

  static int32_t idx(BassParam p) { return static_cast<int32_t>(p); }
  float value(BassParam p) const { return values_[idx(p)]; }

  // Amplitude envelope. Applied squared (exponential-ish tail, as Kick), so sustain is fed as
  // its square root: the public `sustain` is the level the note actually holds at.
  AdsrParams ampParams() const {
    return AdsrParams{value(BassParam::Attack), value(BassParam::Decay),
                      std::sqrt(value(BassParam::Sustain)), value(BassParam::Release)};
  }
  // Internal timbre envelope: same attack/release, but it closes faster and rests lower than the
  // level does, which is what makes the note "pluck" in timbre rather than only in volume.
  AdsrParams timbreParams() const {
    return AdsrParams{value(BassParam::Attack), value(BassParam::Decay) * 0.5f,
                      value(BassParam::Sustain) * 0.4f, value(BassParam::Release)};
  }

  static double semitonesOf(double ratio) { return 12.0 * std::log2(ratio); }

  // Moves toward `target` semitones: at once when slide is 0, else linearly in pitch over `slide`
  // seconds from wherever a glide in progress has got to (constant rate = same speed per octave).
  void startGlide(double target) {
    targetSemis_ = target;
    const double frames = static_cast<double>(value(BassParam::Slide)) * core_.sampleRate;
    if (frames < 1.0 || target == pitchSemis_) {
      pitchSemis_ = target;
      glideStep_ = 0.0;
    } else {
      glideStep_ = (target - pitchSemis_) / frames;
    }
  }

  void clearState() {
    phase_ = 0.25;
    subPhase_ = 0.25;
    dcX1_ = dcY1_ = 0.0f;
    thump_ = 0.0f;
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

  float values_[kBassParamCount] = {};
  PercCore core_;
  bool tied_ = false;  // set by the engine just before noteOn (Instrument::setTie)
  AdsrEnvelope ampEnv_, timbreEnv_;
  BiquadFilter filter_{48000.0};
  double phase_ = 0.25, subPhase_ = 0.25;
  float dcX1_ = 0.0f, dcY1_ = 0.0f, dcBlockR_ = 0.999f;
  float thump_ = 0.0f;
  double pitchSemis_ = 0.0, targetSemis_ = 0.0, glideStep_ = 0.0;  // semitones above Tune
  float lastCutoff_ = -1.0f;
  float qComp_ = 1.0f;
  bool qDirty_ = true;
};

}  // namespace webdsp
