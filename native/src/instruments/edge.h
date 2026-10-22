#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/dual_filter.h"
#include "../dsp/edge_osc.h"
#include "../dsp/fm_kernel.h"
#include "../dsp/percussion.h"

// Synthesized "Edge": clean, sharp, transient-focused plucks, stabs and leads. Behaviour,
// parameter mapping and sound-design recipes: docs/edge-instrument.md. An original design, not a
// model of any instrument.
//
//   band-limited oscillator (+ optional detuned copy)        } shape (saw -> square -> pulse)
//      brightness = harmonic count 1..256, swept by the tone envelope   } bright, brightenv
//      pitch = tune x note x 2^(pitch transient + vibrato)              } pitchenv/pitchdecay, lfopitch
//        -> DualFilter: A, B in {LP12, LP24, HP12, BP}; single / series / parallel
//           cutoff A = cutoff x keytrack x 2^(tone env x filterenv + lfo x lfofilter); B = A x 2^spread
//        -> x amp env^2 x velocity x resonance compensation -> soft clip (bounded +-1) -> bus
//
// What makes it a different instrument rather than "oscillator -> resonant filter":
//  - Brightness lives in the oscillator (a harmonic count, alias-free up to Nyquist), so a pluck
//    gets its bite from a bright onset that settles, not from a resonant filter or drive. There is
//    no drive stage at all.
//  - A signed pitch transient (ms-scale) gives the "zap"/"bloop" that makes a short note read as a
//    pluck even with an open, flat filter.
//  - Two filter stages with routing and band-pass: a window (LP then HP) or a scoop (LP + HP in
//    parallel) are shapes no single low-pass produces. Series resonance is tied (see DualFilter).
//
// Reused DSP: AdsrEnvelope (amplitude), PercEnv (pitch and tone envelopes), PercCore (lifecycle/
// pan/routing), BiquadFilter and PercBandpass (inside DualFilter), fmk sine table (LFO),
// setTableParam. New: dsp/edge_osc.h (tables) and dsp/dual_filter.h.
//
// Mono per instance; a retrigger is continuous (phases, filter state and amplitude level carry; the
// transient and tone envelopes restart); a fresh note resets everything so identical inputs render
// identical samples. Not tempo-synced. No allocation after configure().
namespace webdsp {

inline constexpr InstrumentParamInfo kEdgeParams[] = {
    {static_cast<int32_t>(EdgeParam::Tune), "tune", 27.5f, 1760.0f, 220.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(EdgeParam::Shape), "shape", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::Bright), "bright", 0.0f, 1.0f, 0.55f, "", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::BrightEnv), "brightenv", -1.0f, 1.0f, 0.4f, "", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::Detune), "detune", 0.0f, 50.0f, 0.0f, "cents", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::PitchEnv), "pitchenv", -24.0f, 24.0f, 3.0f, "st", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::PitchDecay), "pitchdecay", 0.002f, 0.4f, 0.025f, "s", ModDomain::Log},
    {static_cast<int32_t>(EdgeParam::Routing), "routing", 0.0f, 2.0f, 1.0f, "step", ModDomain::None},
    {static_cast<int32_t>(EdgeParam::FilterA), "filtera", 0.0f, 3.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(EdgeParam::FilterB), "filterb", 0.0f, 3.0f, 2.0f, "step", ModDomain::None},
    {static_cast<int32_t>(EdgeParam::Cutoff), "cutoff", 40.0f, 16000.0f, 3000.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(EdgeParam::Spread), "spread", -4.0f, 4.0f, -3.0f, "oct", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::Resonance), "resonance", 0.0f, 1.0f, 0.1f, "", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::FilterEnv), "filterenv", -1.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::ToneDecay), "tonedecay", 0.01f, 2.0f, 0.18f, "s", ModDomain::Log},
    {static_cast<int32_t>(EdgeParam::LfoRate), "lforate", 0.1f, 20.0f, 5.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(EdgeParam::LfoPitch), "lfopitch", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::LfoFilter), "lfofilter", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(EdgeParam::Attack), "attack", 0.0005f, 0.25f, 0.001f, "s"},
    {static_cast<int32_t>(EdgeParam::Decay), "decay", 0.01f, 4.0f, 0.3f, "s"},
    {static_cast<int32_t>(EdgeParam::Sustain), "sustain", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(EdgeParam::Release), "release", 0.005f, 2.0f, 0.08f, "s"},
};
inline constexpr int32_t kEdgeParamCount =
    static_cast<int32_t>(sizeof(kEdgeParams) / sizeof(kEdgeParams[0]));

class EdgeInstrument final : public Instrument {
 public:
  EdgeInstrument() {
    for (int32_t i = 0; i < kEdgeParamCount; i++) values_[i] = kEdgeParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Edge; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    ampEnv_.configure(sampleRate);
    ampEnv_.reset();
    pitchEnv_.configure(sampleRate);
    toneEnv_.configure(sampleRate);
    sine_ = fmk::sineTable();
    tables_ = &edge::tables();  // built here, never in render()
    filter_.configure(sampleRate);
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    if (core_.begin(velocity, delayFrames, pan, pitch)) clearState();  // retrigger carries state
    ampEnv_.noteOnRestart(ampParams());
    pitchCoef_ = percDecayCoef(value(EdgeParam::PitchDecay) * kDecayToTau, core_.sampleRate);
    pitchEnv_.trigger(pitchCoef_, kInstantAttack);
    toneEnv_.trigger(percDecayCoef(value(EdgeParam::ToneDecay) * kDecayToTau, core_.sampleRate), kInstantAttack);
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    ampEnv_.noteOff(ampParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kEdgeParams, kEdgeParamCount, values_, param, v);
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

    const float velocity = std::min(core_.velocity, 1.0f);
    // Velocity is mostly level; hard hits are also a little brighter and open the filter further.
    const float vTone = 0.6f + 0.4f * velocity;
    const float shapeKnob = value(EdgeParam::Shape);
    const float bright = value(EdgeParam::Bright);
    const float brightEnv = value(EdgeParam::BrightEnv);
    const float detuneCents = value(EdgeParam::Detune);
    const float osc2 = std::min(detuneCents * 0.1f, 1.0f);       // fades in over the first 10 cents
    const float osc2Norm = 1.0f / std::sqrt(1.0f + osc2 * osc2);
    const double detuneRatio = std::exp2(static_cast<double>(detuneCents) / 1200.0);
    const float pitchEnvSt = value(EdgeParam::PitchEnv);
    const double fRest = static_cast<double>(value(EdgeParam::Tune)) * core_.pitchRatio;
    const double incRest = fRest / sr;
    const double incCap = 0.45;                                  // instantaneous frequency < 0.45 fs
    const float lfoPitchSt = value(EdgeParam::LfoPitch) * kLfoPitchSemitones;
    const float lfoFilterOct = value(EdgeParam::LfoFilter) * kLfoFilterOctaves;
    const double lfoInc = static_cast<double>(value(EdgeParam::LfoRate)) / sr;

    const float cutoff = value(EdgeParam::Cutoff);
    const float spread = value(EdgeParam::Spread);
    const float filterEnv = value(EdgeParam::FilterEnv);
    const float keytrack = static_cast<float>(std::pow(core_.pitchRatio, kKeytrack));
    filter_.setRouting(static_cast<int32_t>(value(EdgeParam::Routing) + 0.5f));
    filter_.setModes(static_cast<int32_t>(value(EdgeParam::FilterA) + 0.5f),
                     static_cast<int32_t>(value(EdgeParam::FilterB) + 0.5f));
    filter_.setResonance(value(EdgeParam::Resonance));
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

      // --- control rate.
      const float tone = toneEnv_.level();
      const float lfo = fmk::sinTurns(sine_, lfoPhase_);
      const float lfoSt = lfoPitchSt * lfo;
      // Highest pitch this step will reach (the transient is monotonic), for the harmonic cap.
      const float levelNow = pitchEnv_.level();
      const float levelEnd = levelNow * std::pow(pitchCoef_, static_cast<float>(n));
      const float stTop = std::max(pitchEnvSt * levelNow, pitchEnvSt * levelEnd) + lfoSt;
      const double incTop = std::min(incRest * std::exp2(static_cast<double>(stTop) / 12.0) *
                                         std::max(detuneRatio, 1.0) * 1.001, incCap);
      const float b = std::clamp(bright + brightEnv * tone * vTone - kVelocityDark * (1.0f - velocity), 0.0f, 1.0f);
      shape_ = edge::makeShape(*tables_, b, shapeKnob, incTop);

      const float octs = filterEnv * kMaxEnvOctaves * tone * vTone + lfoFilterOct * lfo;
      const float cutA = cutoff * keytrack * std::exp2(octs);
      filter_.setCutoffs(cutA, cutA * std::exp2(spread));
      const float comp = filter_.gainComp();

      // --- audio rate.
      for (int32_t k = 0; k < n; k++) {
        const float sw = pitchEnv_.next();  // 1 on the first sample of a hit, falling to 0
        const double inc1 = std::min(incRest * std::exp2(static_cast<double>(pitchEnvSt * sw + lfoSt) / 12.0), incCap);
        phase1_ += inc1;
        phase1_ -= std::floor(phase1_);
        float s = edge::read(shape_, phase1_);
        if (osc2 > 0.0f) {
          phase2_ += std::min(inc1 * detuneRatio, incCap);
          phase2_ -= std::floor(phase2_);
          s = (s + osc2 * edge::read(shape_, phase2_)) * osc2Norm;
        }
        mono[i + k] = s;
        toneEnv_.next();
        const float e = ampEnv_.next();
        gains[i + k] = e * e * levelGain * comp;
      }
      lfoPhase_ += lfoInc * n;
      lfoPhase_ -= std::floor(lfoPhase_);

      filter_.process(mono + i, n);
      i += n;
    }

    for (int32_t k = lead; k < numFrames; k++) mono[k] = softClip(mono[k] * gains[k]);

    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr float kLevel = 0.9f;
  static constexpr int32_t kSubBlock = 16;        // control-rate step (~0.33 ms at 48 kHz)
  static_assert(kSubBlock <= DualFilter::kMaxBlock, "control step must fit the filter block");
  static constexpr float kInstantAttack = 1e-6f;  // PercEnv clamps to one frame
  static constexpr float kMaxEnvOctaves = 6.0f;
  static constexpr float kKeytrack = 0.7f;        // cutoff follows note pitch ^ 0.7
  static constexpr float kLfoPitchSemitones = 1.5f;
  static constexpr float kLfoFilterOctaves = 3.0f;
  static constexpr float kVelocityDark = 0.15f;   // soft hits are up to 0.15 darker (of the knob)

  static int32_t idx(EdgeParam p) { return static_cast<int32_t>(p); }
  float value(EdgeParam p) const { return values_[idx(p)]; }

  // Amplitude envelope; applied squared, so sustain is fed as its square root (as Bass/PWM Beast).
  AdsrParams ampParams() const {
    return AdsrParams{value(EdgeParam::Attack), value(EdgeParam::Decay),
                      std::sqrt(value(EdgeParam::Sustain)), value(EdgeParam::Release)};
  }

  void clearState() {
    phase1_ = 0.0;
    phase2_ = 0.37;  // apart from osc 1 so the pair never starts phase-locked
    lfoPhase_ = 0.0;
    pitchEnv_.reset();
    toneEnv_.reset();
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

  float values_[kEdgeParamCount] = {};
  PercCore core_;
  AdsrEnvelope ampEnv_;
  PercEnv pitchEnv_, toneEnv_;
  DualFilter filter_;
  edge::Shape shape_;
  const edge::Tables* tables_ = nullptr;
  const float* sine_ = nullptr;
  double phase1_ = 0.0, phase2_ = 0.37, lfoPhase_ = 0.0;
  float pitchCoef_ = 0.9f;
};

}  // namespace webdsp
