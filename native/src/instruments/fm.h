#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/biquad_filter.h"
#include "../dsp/filter_modulation.h"
#include "../dsp/percussion.h"

// Synthesized FM voice: two-operator phase modulation with an internal index envelope.
// Design rationale: docs/fm-instrument-proposal.md; shipped behaviour: docs/fm-instrument.md.
//
//   modulator (sine, internal self-feedback) --index(t)--> phase of carrier (sine)
//        freq = ratio x carrier                              freq = note
//                                   |
//                    BiquadFilter LP 12 dB (static tone, cutoff = fundamental x 2^(1 + 7 tone))
//                                   |
//              x amp env^2 x velocity -> soft clip -> Panner -> bus
//
// What makes it FM rather than "oscillator -> filter": brightness is *generated* (sidebands) by an
// index that follows an internal envelope (bright onset, settles clean) and velocity, and loudness
// is independent of it (PM has constant energy). A low-pass cannot make the hollow (odd-harmonic)
// or inharmonic colours the stepped `ratio` gives.
//
// The modulator frequency is always derived from the live carrier frequency (ratio x note, also
// during a glide), so the spectrum keeps its shape across the keyboard. The effective index is
// bandwidth-limited ((index + 1) x modulator Hz < 0.4 fs) so high notes and high ratios cannot
// alias, at any sample rate. At index 0 the output is exactly the carrier sine.
//
// Reused: AdsrEnvelope (x2: amplitude + internal index envelope), BiquadFilter, FilterModulation
// (gated cutoff updates only, depth 0), PercCore. Mono per instance; retrigger is continuous;
// legato/slide follow Bass (see bass.h).
namespace webdsp {

// Stepped modulator-to-carrier ratios (the `ratio` param is an index into this table).
inline constexpr float kFmRatios[] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 1.41421356f, 3.5f};
inline constexpr int32_t kFmRatioCount = static_cast<int32_t>(sizeof(kFmRatios) / sizeof(kFmRatios[0]));

inline constexpr InstrumentParamInfo kFmParams[] = {
    {static_cast<int32_t>(FmParam::Tune), "tune", 27.5f, 880.0f, 110.0f, "Hz"},
    {static_cast<int32_t>(FmParam::Ratio), "ratio", 0.0f, static_cast<float>(kFmRatioCount - 1), 1.0f, "step"},
    {static_cast<int32_t>(FmParam::Timbre), "timbre", 0.0f, 1.0f, 0.35f, ""},
    {static_cast<int32_t>(FmParam::Snap), "snap", 0.0f, 1.0f, 0.6f, ""},
    {static_cast<int32_t>(FmParam::Tone), "tone", 0.0f, 1.0f, 0.75f, ""},
    {static_cast<int32_t>(FmParam::Attack), "attack", 0.001f, 0.25f, 0.003f, "s"},
    {static_cast<int32_t>(FmParam::Decay), "decay", 0.03f, 2.0f, 0.3f, "s"},
    {static_cast<int32_t>(FmParam::Sustain), "sustain", 0.0f, 1.0f, 0.6f, ""},
    {static_cast<int32_t>(FmParam::Release), "release", 0.01f, 1.5f, 0.1f, "s"},
    {static_cast<int32_t>(FmParam::Legato), "legato", 0.0f, 1.0f, 0.0f, "", ModDomain::None},
    {static_cast<int32_t>(FmParam::Slide), "slide", 0.0f, 0.5f, 0.0f, "s"},
};
inline constexpr int32_t kFmParamCount =
    static_cast<int32_t>(sizeof(kFmParams) / sizeof(kFmParams[0]));

class FmInstrument final : public Instrument {
 public:
  FmInstrument() {
    for (int32_t i = 0; i < kFmParamCount; i++) values_[i] = kFmParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Fm; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    ampEnv_.configure(sampleRate);
    indexEnv_.configure(sampleRate);
    ampEnv_.reset();
    indexEnv_.reset();
    filter_ = BiquadFilter(sampleRate);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterSlope), 12.0f);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), kFilterQ);
    filterMod_.configure(sampleRate);
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    const bool legato = value(FmParam::Legato) >= 0.5f && core_.active && !core_.releasing &&
                        core_.pendingDelay == 0 && delayFrames <= 0;
    if (legato) {
      const float heldVelocity = core_.velocity;
      core_.begin(velocity, 0, pan, pitch);
      core_.velocity = heldVelocity;  // a velocity step would be an audible level/brightness jump
      startGlide(semitonesOf(core_.pitchRatio));
      return;
    }
    const bool fresh = core_.begin(velocity, delayFrames, pan, pitch);
    if (fresh) clearState();
    pitchSemis_ = targetSemis_ = semitonesOf(core_.pitchRatio);
    if (fresh) filterSemis_ = pitchSemis_;
    glideStep_ = 0.0;
    ampEnv_.noteOnRestart(ampParams());
    indexEnv_.noteOn(indexParams());
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    ampEnv_.noteOff(ampParams());
    indexEnv_.noteOff(indexParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kFmParams, kFmParamCount, values_, param, v);
  }

  void reset() override {
    ampEnv_.reset();
    indexEnv_.reset();
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

    const double tuneHz = static_cast<double>(value(FmParam::Tune));
    const int32_t ratioIdx = std::clamp(static_cast<int32_t>(std::lround(value(FmParam::Ratio))), 0,
                                        kFmRatioCount - 1);
    const double ratio = static_cast<double>(kFmRatios[ratioIdx]);
    const float timbre = value(FmParam::Timbre);
    const float snap = value(FmParam::Snap);
    const float tone = value(FmParam::Tone);
    // Velocity always brightens a little (harder = more sidebands); it does not change level shape.
    const float vf = 0.55f + 0.45f * std::min(core_.velocity, 1.0f);
    const float restIndex = kRestIndex * std::pow(timbre, 1.4f);
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

      // --- control rate (once per sub-block): pitch, index, filter.
      if (pitchSemis_ != targetSemis_) {
        pitchSemis_ += glideStep_ * n;
        if (glideStep_ > 0.0 ? pitchSemis_ >= targetSemis_ : pitchSemis_ <= targetSemis_) {
          pitchSemis_ = targetSemis_;
        }
      }
      const double f0 = std::clamp(tuneHz * std::exp2(pitchSemis_ / 12.0), 20.0,
                                   std::min(4000.0, sr * 0.2));
      const double incC = f0 / sr;
      const double incM = incC * ratio;

      // Index = resting (timbre) + snap x internal index envelope, scaled by velocity, then
      // limited so the sidebands stay below 0.4 fs (Carson: bandwidth ~ (index + 1) x fm).
      const float wanted = vf * (restIndex + kSnapIndex * snap * indexEnv_.level());
      const double maxIndex = std::max(0.0, 0.4 * sr / (f0 * ratio) - 1.0);
      const float target = static_cast<float>(std::min(static_cast<double>(wanted), maxIndex));
      if (index_ < 0.0f) index_ = target;  // first sub-block of a fresh note: no ramp from 0
      const float indexStep = (target - index_) / static_cast<float>(n);

      // Bass weight: at low pitches a fixed share of the clean carrier is blended in. PM's
      // fundamental passes through Bessel zeros as the index moves (J0 -> 0 near 2.4, 5.5; it also
      // goes negative), so without it a bass note loses its fundamental mid-sweep, and the snap
      // envelope sweeps through that region on every note. 0.5 at/below 80 Hz, fading to 0 at 320 Hz
      // (two octaves, log-linear). The mix is normalised so index 0 is still exactly the carrier.
      const double weightT = std::clamp(1.0 - std::log2(f0 / 80.0) * 0.5, 0.0, 1.0);
      const float weightTarget = static_cast<float>(kBassWeight * weightT);
      if (weight_ < 0.0f) weight_ = weightTarget;
      const float weightStep = (weightTarget - weight_) / static_cast<float>(n);

      // The tone filter follows the pitch at a bounded rate: a note landing on a sounding voice
      // (retrigger on a held or releasing note) jumps the pitch, and a biquad whose cutoff jumps
      // by a fifth in one step clicks on bright FM content. 1 semitone per sub-block = 12 st in 4 ms.
      filterSemis_ += std::clamp(pitchSemis_ - filterSemis_, -kFilterTrackStep, kFilterTrackStep);
      const double fFilter = std::clamp(tuneHz * std::exp2(filterSemis_ / 12.0), 20.0,
                                        std::min(4000.0, sr * 0.2));

      filterMod_.apply(filter_, static_cast<float>(fFilter * std::exp2(1.0 + 7.0 * tone)), 0.0f, 0.0f);

      // --- audio rate.
      for (int32_t k = 0; k < n; k++) {
        phaseM_ += incM;
        phaseM_ -= std::floor(phaseM_);
        phaseC_ += incC;
        phaseC_ -= std::floor(phaseC_);
        index_ += indexStep;

        // Self-feedback grows with the index (0 at index 0, so zero index is a pure sine) and is
        // averaged over two samples for stability. Stays well below the chaotic region.
        const float beta = kFeedbackMax * std::min(index_ * (1.0f / kFeedbackKnee), 1.0f);
        const float m = static_cast<float>(
            std::sin(kTwoPi * phaseM_ + static_cast<double>(beta * 0.5f * (fb1_ + fb2_))));
        fb2_ = fb1_;
        fb1_ = m;
        float y = static_cast<float>(std::sin(kTwoPi * phaseC_ + static_cast<double>(index_ * m)));
        weight_ += weightStep;
        if (weight_ > 0.0f) {
          y = (y + weight_ * static_cast<float>(std::sin(kTwoPi * phaseC_))) * (1.0f / (1.0f + weight_));
        }
        mono[i + k] = y;

        const float e = ampEnv_.next();
        indexEnv_.next();
        gains[i + k] = e * e * levelGain;
      }
      index_ = target;  // land exactly (removes float drift)
      weight_ = weightTarget;

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
  static constexpr float kLevel = 0.5f;  // FM has ~2x the RMS of Bass/Pulse at equal peak; sits lower for mix headroom
  static constexpr int32_t kSubBlock = 16;       // control-rate step (~0.33 ms at 48 kHz)
  static constexpr double kTwoPi = 6.283185307179586;
  static constexpr float kRestIndex = 5.0f;      // radians of peak phase deviation at timbre 1
  static constexpr float kSnapIndex = 7.0f;      // extra radians at note start at snap 1
  static constexpr float kFeedbackMax = 1.1f;    // modulator self-feedback at full index
  static constexpr float kFeedbackKnee = 5.0f;   // index at which feedback reaches its maximum
  static constexpr float kBassWeight = 0.5f;     // clean-carrier share at/below 80 Hz
  static constexpr double kFilterTrackStep = 1.0;  // semitones per sub-block
  static constexpr float kFilterQ = 0.7071f;
  static constexpr float kIndexDecayRatio = 0.35f;

  static int32_t idx(FmParam p) { return static_cast<int32_t>(p); }
  float value(FmParam p) const { return values_[idx(p)]; }

  // Amplitude envelope: squared (exponential-ish tail), sustain fed as its square root so the
  // public `sustain` is the level the note actually holds at (same as Bass).
  AdsrParams ampParams() const {
    return AdsrParams{value(FmParam::Attack), value(FmParam::Decay),
                      std::sqrt(value(FmParam::Sustain)), value(FmParam::Release)};
  }
  // Internal index envelope: same attack/release, a faster decay to zero. `snap` scales it.
  AdsrParams indexParams() const {
    return AdsrParams{value(FmParam::Attack), value(FmParam::Decay) * kIndexDecayRatio, 0.0f,
                      value(FmParam::Release)};
  }

  static double semitonesOf(double ratio) { return 12.0 * std::log2(ratio); }

  void startGlide(double target) {
    targetSemis_ = target;
    const double frames = static_cast<double>(value(FmParam::Slide)) * core_.sampleRate;
    if (frames < 1.0 || target == pitchSemis_) {
      pitchSemis_ = target;
      glideStep_ = 0.0;
    } else {
      glideStep_ = (target - pitchSemis_) / frames;
    }
  }

  void clearState() {
    phaseC_ = 0.25;  // carrier peak: punch from the first cycle; the >= 1 ms attack keeps it click-free
    phaseM_ = 0.0;
    fb1_ = fb2_ = 0.0f;
    index_ = -1.0f;
    weight_ = -1.0f;
    filterSemis_ = 0.0;
    filterMod_.reset();
    filter_.reset();
  }

  // Transparent below 0.6, then a smooth knee bounded by exactly 1 (as Bass).
  static float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.6f) return x;
    const float u = std::min((a - 0.6f) * 2.5f, 3.0f);
    const float y = 0.6f + 0.4f * (u * (27.0f + u * u) / (27.0f + 9.0f * u * u));
    const float c = std::min(y, 1.0f);
    return x < 0.0f ? -c : c;
  }

  float values_[kFmParamCount] = {};
  PercCore core_;
  AdsrEnvelope ampEnv_, indexEnv_;
  BiquadFilter filter_{48000.0};
  FilterModulation filterMod_;
  double phaseC_ = 0.25, phaseM_ = 0.0;
  float fb1_ = 0.0f, fb2_ = 0.0f;
  float index_ = -1.0f;   // current modulation index; < 0 = not yet initialised for this note
  float weight_ = -1.0f;  // current bass-weight share; < 0 = not yet initialised
  double filterSemis_ = 0.0;  // pitch the tone filter is tracking (semitones above Tune)
  double pitchSemis_ = 0.0, targetSemis_ = 0.0, glideStep_ = 0.0;  // semitones above Tune
};

}  // namespace webdsp
