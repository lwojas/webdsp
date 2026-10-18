#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/biquad_filter.h"
#include "../dsp/percussion.h"

// Synthesized hypersaw: seven detuned saws as one instrument. Shipped behaviour and parameter
// mapping: docs/hypersaw-instrument.md.
//
//   7x saw (PolyBLEP): symmetric detune around a centre oscillator            } detune (cents)
//        -> centre-vs-sides mix (power-normalised)                            } density
//        -> constant-power pan per oscillator, outer ones widest              } spread (+ detune)
//        -> 12 dB low-pass (stereo, keytracked)                               } cutoff
//        -> x ADSR (amp) x velocity -> soft clip (bounded +-1) -> balance Panner -> bus
//
// The seven oscillators are one voice, not seven voices: a single envelope, filter and note
// lifecycle, and their detune, level and pan are all derived from three controls. The tables
// below (kOffset, kPan, kStartPhase) are indexed by oscillator and sized by kOsc, so the layout
// is data, not code.
//
// Reused DSP: AdsrEnvelope (amplitude), BiquadFilter (12 dB LP), PercCore (note lifecycle, pan,
// pre-note delay), setTableParam. The PolyBLEP saw and soft clip are the same inline forms Pad
// and Bass use.
//
// Mono per instance (a chord = several instances, as for every instrument). Phase: a fresh note
// starts every oscillator at a fixed, closely grouped phase (see kStartPhase: grouped so detune 0
// is a full-strength saw rather than a phase-cancelled one), so identical inputs render identical
// samples. A
// retrigger on a sounding instrument carries phases, filter and envelope level (no reset, so no
// discontinuity). Not tempo-synced.
namespace webdsp {

inline constexpr InstrumentParamInfo kHypersawParams[] = {
    {static_cast<int32_t>(HypersawParam::Tune), "tune", 27.5f, 440.0f, 110.0f, "Hz"},
    {static_cast<int32_t>(HypersawParam::Detune), "detune", 0.0f, 100.0f, 25.0f, "ct"},
    {static_cast<int32_t>(HypersawParam::Spread), "spread", 0.0f, 1.0f, 0.8f, ""},
    {static_cast<int32_t>(HypersawParam::Density), "density", 0.0f, 1.0f, 0.7f, ""},
    {static_cast<int32_t>(HypersawParam::Cutoff), "cutoff", 200.0f, 16000.0f, 5000.0f, "Hz"},
    {static_cast<int32_t>(HypersawParam::Attack), "attack", 0.001f, 4.0f, 0.01f, "s"},
    {static_cast<int32_t>(HypersawParam::Decay), "decay", 0.02f, 4.0f, 0.5f, "s"},
    {static_cast<int32_t>(HypersawParam::Sustain), "sustain", 0.0f, 1.0f, 0.8f, ""},
    {static_cast<int32_t>(HypersawParam::Release), "release", 0.01f, 6.0f, 0.4f, "s"},
};
inline constexpr int32_t kHypersawParamCount =
    static_cast<int32_t>(sizeof(kHypersawParams) / sizeof(kHypersawParams[0]));

class HypersawInstrument final : public Instrument {
 public:
  HypersawInstrument() {
    for (int32_t i = 0; i < kHypersawParamCount; i++) values_[i] = kHypersawParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Hypersaw; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    env_.configure(sampleRate);
    env_.reset();
    filter_ = BiquadFilter(sampleRate);
    filter_.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), kFilterQ);
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    // A fresh note starts from known phases and a clean filter; a retrigger carries everything.
    if (core_.begin(velocity, delayFrames, pan, pitch)) clearState();
    env_.noteOn(ampParams());
  }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    env_.noteOff(ampParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kHypersawParams, kHypersawParamCount, values_, param, v);
  }

  void reset() override {
    env_.reset();
    clearState();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* left = scratch[0];
    float* right = scratch[1];
    const double sr = core_.sampleRate;

    // --- per call (all of this is constant within a block): frequencies, gains, cutoff.
    const double f0 = std::clamp(static_cast<double>(value(HypersawParam::Tune)) * core_.pitchRatio,
                                 20.0, std::min(2000.0, sr * 0.2));
    const float detune = value(HypersawParam::Detune);
    const float density = value(HypersawParam::Density);
    double inc[kOsc];
    float gl[kOsc], gr[kOsc];
    layout(f0, sr, detune, value(HypersawParam::Spread), density, inc, gl, gr);

    const float keytrack = static_cast<float>(std::sqrt(core_.pitchRatio));  // half-octave per octave
    const float cut = std::clamp(value(HypersawParam::Cutoff) * keytrack, 30.0f,
                                 static_cast<float>(std::min(18000.0, sr * 0.45)));
    if (std::fabs(cut - lastCutoff_) > 0.002f * cut) {
      filter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), cut);
      lastCutoff_ = cut;
    }
    const float levelGain = core_.velocity * kLevel;

    // Pre-note delay: silence, and nothing advances.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t k = 0; k < lead; k++) left[k] = right[k] = 0.0f;
    core_.pendingDelay -= lead;

    bool finished = false;
    int32_t i = lead;
    while (i < numFrames) {
      const int32_t n = std::min(kSubBlock, numFrames - i);
      if (env_.stage() == AdsrEnvelope::Stage::Idle ||
          (env_.stage() == AdsrEnvelope::Stage::Sustain && env_.level() <= 0.0f)) {
        finished = true;
        for (int32_t k = i; k < numFrames; k++) left[k] = right[k] = 0.0f;
        break;
      }

      // --- audio rate: seven saws -> stereo mix.
      for (int32_t k = 0; k < n; k++) {
        float l = 0.0f, r = 0.0f;
        for (int32_t j = 0; j < kOsc; j++) {
          const float saw = sawBlep(phase_[j], inc[j]);
          phase_[j] += inc[j];
          phase_[j] -= std::floor(phase_[j]);
          l += saw * gl[j];
          r += saw * gr[j];
        }
        left[i + k] = l;
        right[i + k] = r;
      }
      float* ch[2] = {left + i, right + i};
      filter_.process(ch, 2, n);

      for (int32_t k = 0; k < n; k++) {
        const float e = env_.next() * levelGain;
        left[i + k] = softClip(left[i + k] * e);
        right[i + k] = softClip(right[i + k] * e);
      }
      i += n;
    }

    const int32_t channels = std::min(busChannelCount, kMaxDspChannels);
    if (channels >= 2) {
      if (core_.panner.isUnity()) {
        for (int32_t k = 0; k < numFrames; k++) {
          busChannels[0][k] += left[k];
          busChannels[1][k] += right[k];
        }
      } else {
        core_.panner.process(left, right, busChannels[0], busChannels[1], numFrames, false, true);
      }
    } else if (channels == 1) {
      for (int32_t k = 0; k < numFrames; k++) busChannels[0][k] += 0.5f * (left[k] + right[k]);
    }

    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr int32_t kOsc = 7;
  static constexpr int32_t kCentre = 3;
  static constexpr int32_t kSubBlock = 32;
  static constexpr float kLevel = 0.5f;
  static constexpr float kFilterQ = 0.7071f;
  // Spread at its designed width needs some detune to be meaningful: the stereo image widens
  // with `detune` from kWidthFloor (at 0 ct) to full at kWidthRefCents, so `spread` is "how much
  // of the width the current detune earns".
  static constexpr float kCoherentCents = 10.0f;
  static constexpr float kWidthFloor = 0.25f;
  static constexpr float kWidthRefCents = 40.0f;
  // Per-oscillator tables, ordered low to high pitch. Detune offsets are in units of `detune`
  // (symmetric; the inner pair at ~0.18 and the middle pair at ~0.57 of the outer pair, the
  // proportions of the classic JP-8000-style stack, so small detune stays calm while the outer
  // pair carries the width). Pan positions are in units of the width, outermost widest.
  static constexpr float kOffset[kOsc] = {-1.0f, -0.57f, -0.18f, 0.0f, 0.18f, 0.57f, 1.0f};
  static constexpr float kPan[kOsc] = {-1.0f, -0.6f, -0.22f, 0.0f, 0.22f, 0.6f, 1.0f};
  // Fixed start phases (notes are reproducible), all within 0.11 of a cycle of each other. Not
  // spread over the whole cycle: at small detune the phase relationship stays frozen, and phases
  // spread evenly would cancel the fundamental (a hollow "unison"). Close phases keep detune 0 a
  // full-strength saw, and any real detune walks the phases apart on its own within a beat.
  static constexpr double kStartPhase[kOsc] = {0.0, 0.074, 0.028, 0.103, 0.057, 0.011, 0.085};

  static int32_t idx(HypersawParam p) { return static_cast<int32_t>(p); }
  float value(HypersawParam p) const { return values_[idx(p)]; }

  AdsrParams ampParams() const {
    return AdsrParams{value(HypersawParam::Attack), value(HypersawParam::Decay),
                      value(HypersawParam::Sustain), value(HypersawParam::Release)};
  }

  // Phase increments and per-channel gains for the seven oscillators.
  //  - Frequency: f0 * 2^(detune * offset / 1200), symmetric about the centre.
  //  - Level (density d): centre 1 - 0.55 d, sides 0.04 + 1.28 d - 0.74 d^2 (the measured shape of
  //    a hardware supersaw mix: d=0 is the centre alone, d=1 puts the sides above it). The seven
  //    are then normalised so density changes character, not loudness. Saws at distinct
  //    frequencies add in power (1/sqrt(sum g^2)); saws at (nearly) the same frequency add in
  //    amplitude (1/sum g) - so the normaliser slides from the second to the first over the first
  //    kCoherentCents of detune, and a unison sounds as loud as a wide stack instead of ~8 dB over.
  //  - Pan: constant-power, centre unity (as Panner mono-source law), positions kPan * width.
  static void layout(double f0, double sr, float detuneCents, float spread, float density,
                     double* inc, float* gl, float* gr) {
    const float d = density;
    const float centre = 1.0f - 0.55f * d;
    const float side = 0.04f + 1.28f * d - 0.74f * d * d;
    const float power = 1.0f / std::sqrt(centre * centre + (kOsc - 1) * side * side);
    const float amplitude = 1.0f / (centre + (kOsc - 1) * side);
    const float x = std::min(1.0f, detuneCents / kCoherentCents);
    const float norm = amplitude + (power - amplitude) * (x * x * (3.0f - 2.0f * x));
    const float width =
        spread * (kWidthFloor + (1.0f - kWidthFloor) * std::min(1.0f, detuneCents / kWidthRefCents));
    for (int32_t j = 0; j < kOsc; j++) {
      inc[j] = f0 * std::exp2(static_cast<double>(detuneCents * kOffset[j]) / 1200.0) / sr;
      const float g = (j == kCentre ? centre : side) * norm;
      const double theta = (static_cast<double>(kPan[j] * width) + 1.0) * 0.7853981633974483;
      gl[j] = g * static_cast<float>(1.4142135623730951 * std::cos(theta));
      gr[j] = g * static_cast<float>(1.4142135623730951 * std::sin(theta));
    }
  }

  void clearState() {
    for (int32_t j = 0; j < kOsc; j++) phase_[j] = kStartPhase[j];
    lastCutoff_ = -1.0f;
    filter_.reset();
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

  // Transparent below 0.6, then a smooth knee that is bounded by exactly 1.
  static float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.6f) return x;
    const float u = std::min((a - 0.6f) * 2.5f, 3.0f);
    const float y = 0.6f + 0.4f * (u * (27.0f + u * u) / (27.0f + 9.0f * u * u));
    const float c = std::min(y, 1.0f);
    return x < 0.0f ? -c : c;
  }

  float values_[kHypersawParamCount] = {};
  PercCore core_;
  AdsrEnvelope env_;
  BiquadFilter filter_{48000.0};
  double phase_[kOsc] = {};
  float lastCutoff_ = -1.0f;
};

}  // namespace webdsp
