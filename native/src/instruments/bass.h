#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
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
    {static_cast<int32_t>(BassParam::EnvMod), "envmod", 0.0f, 1.0f, 0.4f, "", ModDomain::Linear},
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
    ampEnv_.reset();
    fenv_ = 0.0f;
    dcBlockR_ = static_cast<float>(1.0 - 6.283185307179586 * kDcCornerHz / sampleRate);
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
    fenvAttacking_ = true;  // rises from where it is (continuous on a retrigger), then falls
    fenvReleasing_ = false;
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
    fenvReleasing_ = true;
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kBassParams, kBassParamCount, values_, param, v);
  }

  void reset() override {
    ampEnv_.reset();
    fenv_ = 0.0f;
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
    const float envDepth = value(BassParam::EnvMod) * kMaxEnvOctaves;
    const double tuneHz = static_cast<double>(value(BassParam::Tune));
    // Oscillator blend: 0 sine (+ sub) -> 0.5 saw -> 1 square.
    const float wSine = std::clamp(1.0f - 2.0f * body, 0.0f, 1.0f);
    const float wSquare = std::clamp(2.0f * body - 1.0f, 0.0f, 1.0f);
    const float wSaw = 1.0f - wSine - wSquare;
    const float subLevel = 0.12f + 0.33f * wSine;
    const float srcNorm = kSourcePeak / (1.0f + subLevel);
    // Accent: velocity scales the envelope depth and the drive. `bite` widens the effect, so a
    // hard note (velocity 1) always bites more than a soft one, and bite 0 keeps it modest.
    const float accentSpread = 0.35f + 0.45f * bite;
    const float vf = 1.0f - accentSpread * (1.0f - std::min(core_.velocity, 1.0f));
    // Drive makes a waveform louder as well as brighter; take that back so BITE is colour, not volume.
    const float biteComp = 1.3f / (1.0f + 2.0f * std::pow(bite, 1.6f));
    const float levelGain = core_.velocity * kLevel * biteComp;
    const float thumpCoef = percDecayCoef(kThumpSeconds, sr);
    // Filter envelope: instant-ish attack, then an exponential fall toward a low rest level. The
    // time constant is a fraction of `decay`, so the filter closes well inside the note.
    const float dtBlock = static_cast<float>(kSubBlock / sr);
    const float tau = std::max(0.004f, value(BassParam::Decay) * 0.18f);
    const float fenvFall = std::exp(-dtBlock / tau);
    const float fenvRest = value(BassParam::Sustain) * 0.25f;
    const float fenvRise = dtBlock / kFenvAttackSeconds;
    const float relTau = std::max(0.003f, value(BassParam::Release) * 0.33f);
    const float fenvRel = std::exp(-dtBlock / relTau);
    const float k = kMaxFeedback * value(BassParam::Resonance);
    // The ladder loses passband level as feedback rises (the 303 does too); give most of it back.
    const float outComp = 1.0f + 0.65f * k;

    // Pre-note delay: silence, and nothing advances.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t j = 0; j < lead; j++) mono[j] = gains[j] = 0.0f;
    core_.pendingDelay -= lead;

    bool finished = false;
    int32_t i = lead;
    while (i < numFrames) {
      const int32_t n = std::min(kSubBlock, numFrames - i);
      if (ampEnv_.stage() == AdsrEnvelope::Stage::Idle ||
          (ampEnv_.stage() == AdsrEnvelope::Stage::Sustain && ampEnv_.level() <= 0.0f)) {
        finished = true;
        for (int32_t j = i; j < numFrames; j++) mono[j] = gains[j] = 0.0f;
        break;
      }

      // --- control rate: filter envelope, then drive, cutoff and pitch thump from it.
      if (fenvReleasing_) {
        fenv_ *= fenvRel;
      } else if (fenvAttacking_) {
        fenv_ += fenvRise * (1.0f - fenv_);
        if (fenv_ >= 0.98f) { fenv_ = 1.0f; fenvAttacking_ = false; }
      } else {
        fenv_ = fenvRest + (fenv_ - fenvRest) * fenvFall;
      }
      const float t = fenv_;
      // Drive into the filter: bite sets how hard the oscillator hits the shaper (rest level is
      // clean, the note onset is gritty and settles), accents push harder.
      const float d = bite * (0.25f + 0.75f * t * vf);
      const float driveDb = -18.0f + 46.0f * std::pow(d, 0.8f);
      const float g = std::pow(10.0f, driveDb * 0.05f);
      const float bias = 0.15f + 0.5f * bite;  // asymmetry: even harmonics
      const float biasTanh = std::tanh(bias);
      const float comp = kSourcePeak / std::max(std::tanh(g * kSourcePeak + bias) - biasTanh, 0.02f);

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

      // Cutoff sweeps in octaves (linear in pitch, as a 1 V/oct filter does). EnvMod alone sets
      // the depth; accents add to it. Never below 0.85 x the fundamental, so the note survives.
      // Bite adds a short extra zap at the very start of the note (t^3 is only high right after onset).
      const float octs = envDepth * t * (0.55f + 0.45f * vf) + 1.3f * bite * t * t * t + 0.8f * (vf - 1.0f);
      const float floorHz = static_cast<float>(0.85 * f0);
      const float cut = std::clamp(std::max(cutoff * keytrack * std::exp2(octs), floorHz), 30.0f, 12000.0f);
      // One-pole coefficient per half-rate step (the ladder runs 2x oversampled).
      const float gl = std::min(1.0f - std::exp(-6.283185307f * cut / static_cast<float>(2.0 * sr)), 0.4f);

      for (int32_t j = 0; j < n; j++) {
        phase_ += inc;
        phase_ -= std::floor(phase_);
        subPhase_ += inc * 0.5;
        subPhase_ -= std::floor(subPhase_);

        const float saw = polyBlepSaw(phase_ + 0.5, inc);
        const float sq = 0.5f * (saw - polyBlepSaw(phase_, inc));  // saw - shifted saw = 50% pulse
        const float sine = static_cast<float>(std::sin(kTwoPi * phase_));
        const float sub = static_cast<float>(std::sin(kTwoPi * subPhase_));
        const float src = srcNorm * (wSaw * saw + wSquare * sq + wSine * sine + subLevel * sub);

        const float shaped = (std::tanh(g * src + bias) - biasTanh) * comp;
        float y = shaped - dcX1_ + dcBlockR_ * dcY1_;
        y = std::clamp(y, -2.0f, 2.0f);
        dcX1_ = shaped;
        dcY1_ = y;

        // 4-pole ladder with tanh in every stage and in the feedback; output from pole 3 (18 dB).
        float out3 = 0.0f;
        for (int32_t h = 0; h < 2; h++) {
          const float u = fastTanh(y * 1.2f - k * ladder_[3]);
          ladder_[0] += gl * (u - fastTanh(ladder_[0]));
          ladder_[1] += gl * (fastTanh(ladder_[0]) - fastTanh(ladder_[1]));
          ladder_[2] += gl * (fastTanh(ladder_[1]) - fastTanh(ladder_[2]));
          ladder_[3] += gl * (fastTanh(ladder_[2]) - fastTanh(ladder_[3]));
          out3 = ladder_[2];
        }
        mono[i + j] = out3 * outComp;

        const float e = ampEnv_.next();
        gains[i + j] = e * e * levelGain;
      }
      i += n;
    }

    for (int32_t j = lead; j < numFrames; j++) mono[j] = softClip(mono[j] * gains[j]);
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
  static constexpr float kMaxEnvOctaves = 5.0f;   // EnvMod 1
  static constexpr float kMaxFeedback = 3.7f;     // ladder feedback at resonance 1 (4 = self-oscillation)
  static constexpr float kFenvAttackSeconds = 0.002f;
  static constexpr float kThumpSemitones = 0.7f;
  static constexpr float kThumpSeconds = 0.012f;

  static int32_t idx(BassParam p) { return static_cast<int32_t>(p); }
  float value(BassParam p) const { return values_[idx(p)]; }

  // Amplitude envelope. Applied squared (exponential-ish tail, as Kick), so sustain is fed as
  // its square root: the public `sustain` is the level the note actually holds at.
  AdsrParams ampParams() const {
    return AdsrParams{value(BassParam::Attack), value(BassParam::Decay),
                      std::sqrt(value(BassParam::Sustain)), value(BassParam::Release)};
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
    for (float& v : ladder_) v = 0.0f;
    fenv_ = 0.0f;
    fenvAttacking_ = true;
    fenvReleasing_ = false;
  }

  static float fastTanh(float x) {
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
  }

  // Saw in -1..1 with its edge at phase 0, PolyBLEP-corrected (`inc` = phase step per sample).
  static float polyBlepSaw(double ph, double inc) {
    ph -= std::floor(ph);
    float saw = static_cast<float>(2.0 * ph - 1.0);
    if (ph < inc) {
      const double x = ph / inc;
      saw -= static_cast<float>(x + x - x * x - 1.0);
    } else if (ph > 1.0 - inc) {
      const double x = (ph - 1.0) / inc;
      saw -= static_cast<float>(x * x + x + x + 1.0);
    }
    return saw;
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
  AdsrEnvelope ampEnv_;
  float ladder_[4] = {};           // filter stage states
  float fenv_ = 0.0f;              // filter envelope level 0..1 (exponential decay)
  bool fenvAttacking_ = true, fenvReleasing_ = false;
  double phase_ = 0.25, subPhase_ = 0.25;
  float dcX1_ = 0.0f, dcY1_ = 0.0f, dcBlockR_ = 0.999f;
  float thump_ = 0.0f;
  double pitchSemis_ = 0.0, targetSemis_ = 0.0, glideStep_ = 0.0;  // semitones above Tune
};

}  // namespace webdsp
