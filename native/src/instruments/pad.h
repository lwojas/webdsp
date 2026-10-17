#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../dsp/adsr_envelope.h"
#include "../dsp/biquad_filter.h"
#include "../dsp/filter_modulation.h"
#include "../dsp/panner.h"
#include "../dsp/percussion.h"

// Synthesized pad: "Drifting Ensemble". Design rationale and parameter mapping: docs/pad-instrument.md.
//
// per note (a "slot"; kSlots of them so a new note never cuts the previous one's release):
//   4x saw (PolyBLEP), detuned apart, each with its own slow detune drift        } movement
//        -> panned across the stereo field, each with its own slow pan drift      } (4 incommensurate
//        -> 12 dB low-pass (stereo), cutoff = tone x 2^(depth x [envelope + drift]) } sine phases)
//        -> x smooth-stepped amplitude envelope x velocity
//   slots summed -> 2-stage allpass diffuser per channel (mixed in by `space`)
//        -> soft clip (bounded +-1) -> balance Panner -> bus
//
// What makes it a pad rather than "oscillator -> filter -> long release":
//  - Overlap is built in. A note landing on a held note moves that one into its release and starts
//    a fresh voice in a free slot, so old and new notes cross-fade instead of the old one being
//    replaced. (Instrument-internal; the engine's one-instrument-one-note contract is unchanged.)
//  - It never stands still. Every saw's detune and pan drift on their own slow cycles, so the
//    beating pattern, the stereo image and (through the filter) the brightness keep evolving.
//  - The filter follows the envelope: a note blooms open as it swells and darkens as it dies away,
//    and the slow drift keeps moving through the release tail.
//  - The envelope is S-shaped at both ends: no sudden start and no abrupt beginning of the fade.
//  - Width and density: the saws sit across the stereo field and `space` adds a short diffusion
//    stage. Generic room/reverb stays outside, on the bus (EffectKind.Reverb).
//
// Reused DSP: AdsrEnvelope, BiquadFilter (12 dB LP, one stereo instance per slot),
// FilterModulation (envelope + drift -> cutoff), Panner (instrument balance), setTableParam.
//
// Free-running, not tempo-synced: nothing here reads the transport. Deterministic: a fresh slot's
// saw phases are fixed and its drift phases derive from the note's pitch only, so identical inputs
// render identical samples while different chord notes drift out of step with each other.
namespace webdsp {

inline constexpr InstrumentParamInfo kPadParams[] = {
    {static_cast<int32_t>(PadParam::Tune), "tune", 27.5f, 440.0f, 110.0f, "Hz"},
    {static_cast<int32_t>(PadParam::Tone), "tone", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(PadParam::Movement), "movement", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(PadParam::Detune), "detune", 0.0f, 1.0f, 0.4f, ""},
    {static_cast<int32_t>(PadParam::Space), "space", 0.0f, 1.0f, 0.6f, ""},
    {static_cast<int32_t>(PadParam::Attack), "attack", 0.02f, 8.0f, 0.8f, "s"},
    {static_cast<int32_t>(PadParam::Release), "release", 0.1f, 12.0f, 3.0f, "s"},
};
inline constexpr int32_t kPadParamCount =
    static_cast<int32_t>(sizeof(kPadParams) / sizeof(kPadParams[0]));

class PadInstrument final : public Instrument {
 public:
  PadInstrument() {
    for (int32_t i = 0; i < kPadParamCount; i++) values_[i] = kPadParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Pad; }

  void configure(double sampleRate) override {
    sampleRate_ = sampleRate;
    panner_.configure(sampleRate);
    for (Slot& s : slots_) {
      s.env.configure(sampleRate);
      s.env.reset();
      s.filter = BiquadFilter(sampleRate);
      s.filter.setParam(static_cast<int32_t>(VoiceParam::FilterResonance), kFilterQ);
      s.mod.configure(sampleRate);
      s.active = s.held = false;
      s.delay = 0;
    }
    for (int32_t c = 0; c < 2; c++) {
      diffuser_[c][0].init(sampleRate * (c == 0 ? 0.0041 : 0.0047), kDiffuserGain);
      diffuser_[c][1].init(sampleRate * (c == 0 ? 0.0067 : 0.0079), kDiffuserGain);
    }
    tailFrames_ = static_cast<int32_t>(sampleRate * kDiffuserTailSeconds);
    tailLeft_ = 0;
    active_ = false;
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    const bool fresh = !active_;
    if (fresh) clearDiffuser();
    panner_.setPan(std::isfinite(pan) ? pan : 0.0f);
    if (fresh) panner_.snap(false);

    // The previous held note goes into its release; it keeps ringing under the new one.
    releaseHeld();

    Slot* s = pickSlot();
    const bool stolen = s->active;
    const float vel = std::isfinite(velocity) ? std::max(0.0f, velocity) : 0.0f;
    const float semis = std::isfinite(pitch) ? std::clamp(pitch, -48.0f, 48.0f) : 0.0f;
    s->velocity = vel;
    s->semis = semis;
    s->pitchRatio = std::exp2(static_cast<double>(semis) / 12.0);
    s->held = true;
    if (stolen) {
      // Taking over the quietest tail: state carries and the envelope attacks from its current
      // level, so the only discontinuity is a pitch step at (by choice of slot) the lowest level.
      s->delay = 0;
    } else {
      s->delay = std::max(0, delayFrames);
      s->active = true;
      startSlot(*s);
    }
    s->env.noteOn(ampParams());
    active_ = true;
    tailLeft_ = tailFrames_;
  }

  void noteOff() override { releaseHeld(); }

  void setParam(int32_t param, float v) override {
    setTableParam(kPadParams, kPadParamCount, values_, param, v);
  }

  void reset() override {
    for (Slot& s : slots_) {
      s.env.reset();
      s.filter.reset();
      s.mod.reset();
      s.active = s.held = false;
      s.delay = 0;
    }
    clearDiffuser();
    tailLeft_ = 0;
    active_ = false;
  }

  bool isActive() const override { return active_; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!active_) return false;
    float* left = scratch[0];
    float* right = scratch[1];
    std::fill(left, left + numFrames, 0.0f);
    std::fill(right, right + numFrames, 0.0f);

    bool any = false;
    for (Slot& s : slots_) {
      if (!s.active) continue;
      renderSlot(s, left, right, numFrames);
      any = any || s.active;
    }

    // Output stage: diffusion (mixed in by `space`), then the bound.
    const float mix = kMaxDiffuserMix * value(PadParam::Space);
    for (int32_t i = 0; i < numFrames; i++) {
      const float dl = left[i], dr = right[i];
      const float wl = diffuser_[0][1].process(diffuser_[0][0].process(dl));
      const float wr = diffuser_[1][1].process(diffuser_[1][0].process(dr));
      left[i] = softClip(dl + mix * (wl - dl));
      right[i] = softClip(dr + mix * (wr - dr));
    }

    const int32_t channels = std::min(busChannelCount, kMaxDspChannels);
    if (channels >= 2) {
      if (panner_.isUnity()) {
        for (int32_t i = 0; i < numFrames; i++) {
          busChannels[0][i] += left[i];
          busChannels[1][i] += right[i];
        }
      } else {
        panner_.process(left, right, busChannels[0], busChannels[1], numFrames, false, true);
      }
    } else if (channels == 1) {
      for (int32_t i = 0; i < numFrames; i++) busChannels[0][i] += 0.5f * (left[i] + right[i]);
    }

    if (any) {
      tailLeft_ = tailFrames_;
    } else {
      tailLeft_ -= numFrames;
      if (tailLeft_ <= 0) active_ = false;
    }
    return active_;
  }

 private:
  static constexpr int32_t kSlots = 3;     // one held note + two release tails
  static constexpr int32_t kSaws = 4;
  static constexpr int32_t kSubBlock = 32;  // control-rate step; everything it drives is slow
  static constexpr double kTwoPi = 6.283185307179586;
  static constexpr float kLevel = 0.8f;
  static constexpr float kSrcNorm = 0.3f;
  static constexpr float kFilterQ = 0.8f;
  static constexpr float kMinCutoffHz = 250.0f;
  static constexpr float kToneOctaves = 5.0f;        // tone 0..1 -> 250 Hz .. 8 kHz
  static constexpr float kEnvShare = 0.6f;           // how much a closed envelope darkens
  static constexpr float kDriftShare = 0.4f;         // how much drift moves the cutoff at movement 1
  static constexpr float kModOctaves = 3.0f;
  static constexpr float kMaxDetuneCents = 22.0f;
  static constexpr float kDetuneWobble = 0.35f;      // relative swing of each saw's detune
  static constexpr float kMaxPanDrift = 0.25f;
  static constexpr float kDiffuserGain = 0.5f;
  static constexpr float kMaxDiffuserMix = 0.6f;
  static constexpr double kDiffuserTailSeconds = 0.04;
  // Per-saw offsets (in units of the maximum detune), pan positions, start phases and the drift
  // cycle rates in Hz. Rates are deliberately unrelated so the pattern never repeats audibly.
  static constexpr float kDetuneOffset[kSaws] = {-1.0f, -0.37f, 0.43f, 1.0f};
  static constexpr float kPanPosition[kSaws] = {-1.0f, -0.35f, 0.35f, 1.0f};
  static constexpr double kStartPhase[kSaws] = {0.0, 0.27, 0.55, 0.81};
  static constexpr double kDriftHz[kSaws] = {0.071, 0.113, 0.169, 0.233};

  static int32_t idx(PadParam p) { return static_cast<int32_t>(p); }
  float value(PadParam p) const { return values_[idx(p)]; }

  // Attack/release only; decay 0 and sustain 1 are private. The linear ramps are bent into an
  // S-curve at the point of use (smoothStep), so both ends of a swell are gentle.
  AdsrParams ampParams() const {
    return AdsrParams{value(PadParam::Attack), 0.0f, 1.0f, value(PadParam::Release)};
  }

  static float smoothStep(float e) { return e * e * (3.0f - 2.0f * e); }

  struct Allpass {
    std::vector<float> buf;
    size_t pos = 0;
    float g = 0.5f;
    void init(double frames, float gain) {
      buf.assign(std::max<size_t>(2, static_cast<size_t>(frames)), 0.0f);
      pos = 0;
      g = gain;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
    float process(float x) {
      const float d = buf[pos];
      const float w = x + g * d;
      buf[pos] = w;
      if (++pos >= buf.size()) pos = 0;
      return d - g * w;
    }
  };

  struct Slot {
    AdsrEnvelope env;
    BiquadFilter filter{48000.0};
    FilterModulation mod;
    bool active = false;
    bool held = false;  // sounding and not yet released
    int32_t delay = 0;  // pre-note silence, frames
    float velocity = 1.0f;
    float semis = 0.0f;
    double pitchRatio = 1.0;
    double phase[kSaws] = {};
    double drift[kSaws] = {};
  };

  void startSlot(Slot& s) {
    s.env.reset();
    s.filter.reset();
    s.mod.reset();
    // Drift phases come from the pitch alone: a chord's notes drift out of step, a repeated
    // note renders identically.
    const double seed = static_cast<double>(s.semis) * 0.6180339887;
    for (int32_t k = 0; k < kSaws; k++) {
      s.phase[k] = kStartPhase[k];
      const double p = seed + 0.37 * k;
      s.drift[k] = p - std::floor(p);
    }
  }

  void releaseHeld() {
    for (Slot& s : slots_) {
      if (!s.active || !s.held) continue;
      s.held = false;
      if (s.delay > 0) {  // released before it ever started: cancelled
        s.active = false;
        s.delay = 0;
        s.env.reset();
        continue;
      }
      s.env.noteOff(ampParams());
    }
  }

  // A free slot, else the quietest one (always a release tail: releaseHeld ran first).
  Slot* pickSlot() {
    Slot* best = &slots_[0];
    for (Slot& s : slots_) {
      if (!s.active) return &s;
      if (s.env.level() < best->env.level()) best = &s;
    }
    return best;
  }

  void clearDiffuser() {
    for (auto& ch : diffuser_) {
      for (Allpass& a : ch) a.clear();
    }
  }

  void renderSlot(Slot& s, float* left, float* right, int32_t numFrames) {
    const double sr = sampleRate_;
    const float tone = value(PadParam::Tone);
    const float movement = value(PadParam::Movement);
    const float detune = value(PadParam::Detune);
    const float width = value(PadParam::Space);
    const double f0 = std::clamp(static_cast<double>(value(PadParam::Tune)) * s.pitchRatio, 20.0,
                                 std::min(2000.0, sr * 0.2));
    const float baseCutoff = kMinCutoffHz * std::exp2(tone * kToneOctaves) *
                             static_cast<float>(std::sqrt(s.pitchRatio));
    const float levelGain = s.velocity * kLevel;

    int32_t i = std::min(s.delay, numFrames);
    s.delay -= i;
    while (i < numFrames) {
      if (!s.env.isActive()) {
        s.active = s.held = false;
        return;
      }
      const int32_t n = std::min(kSubBlock, numFrames - i);

      // --- control rate: drift phases -> per-saw increment and gains, and the cutoff.
      double inc[kSaws];
      float gl[kSaws], gr[kSaws];
      float sines[kSaws];
      for (int32_t k = 0; k < kSaws; k++) {
        s.drift[k] += kDriftHz[k] * n / sr;
        s.drift[k] -= std::floor(s.drift[k]);
        sines[k] = static_cast<float>(std::sin(kTwoPi * s.drift[k]));
      }
      for (int32_t k = 0; k < kSaws; k++) {
        const float cents = kMaxDetuneCents * detune * kDetuneOffset[k] *
                            (1.0f + kDetuneWobble * movement * sines[k]);
        inc[k] = f0 * std::exp2(static_cast<double>(cents) / 1200.0) / sr;
        const float p = std::clamp(
            kPanPosition[k] * width + kMaxPanDrift * movement * sines[(k + 1) % kSaws], -1.0f, 1.0f);
        const double theta = (static_cast<double>(p) + 1.0) * kTwoPi / 8.0;
        gl[k] = static_cast<float>(1.4142135623730951 * std::cos(theta)) * kSrcNorm;
        gr[k] = static_cast<float>(1.4142135623730951 * std::sin(theta)) * kSrcNorm;
      }
      // Envelope + drift is the pad's modulation signal: closed envelope darkens, drift moves
      // either way (shared filter modulation, dsp/filter_modulation.h). Floor at the fundamental.
      const float signal = -kEnvShare * (1.0f - s.env.level()) +
                           kDriftShare * movement * 0.5f * (sines[1] + sines[2]);
      s.mod.apply(s.filter, baseCutoff, kModOctaves, signal, static_cast<float>(f0));

      // --- audio rate: saws -> stereo mix.
      float tl[kSubBlock], tr[kSubBlock];
      for (int32_t k = 0; k < n; k++) {
        float l = 0.0f, r = 0.0f;
        for (int32_t j = 0; j < kSaws; j++) {
          const float saw = sawBlep(s.phase[j], inc[j]);
          s.phase[j] += inc[j];
          s.phase[j] -= std::floor(s.phase[j]);
          l += saw * gl[j];
          r += saw * gr[j];
        }
        tl[k] = l;
        tr[k] = r;
      }
      float* ch[2] = {tl, tr};
      s.filter.process(ch, 2, n);

      for (int32_t k = 0; k < n; k++) {
        const float e = smoothStep(s.env.next()) * levelGain;
        left[i + k] += tl[k] * e;
        right[i + k] += tr[k] * e;
      }
      i += n;
    }
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

  float values_[kPadParamCount] = {};
  double sampleRate_ = 48000.0;
  Slot slots_[kSlots];
  Allpass diffuser_[2][2];  // [channel][stage]
  Panner panner_;
  bool active_ = false;
  int32_t tailFrames_ = 1920, tailLeft_ = 0;
};

}  // namespace webdsp
