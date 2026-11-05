#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/percussion.h"

// Noise: a synthesis engine whose primary sound source is noise. Behaviour, parameter mapping and
// sound-design recipes: docs/noise-instrument.md.
//
//   source (white / pink / brown / metal / crackle / burst)
//     --> colour: one-knob spectral tilt (dark rumble .. balanced .. bright hat)
//     --> texture: stochastic grain gate (smooth .. grainy .. granular .. crackling), rate = density
//     --+--> dry -------------------------------------------+
//       +--> six-band resonator bank (ratio table morphs     +--> equal-power mix --> drive (tanh, bounded
//            unison / harmonic / bell / cluster; Q = ring)  -+     regenerative feedback into the bank)
//     --> DC block --> tone tilt --> x amp env^2 x velocity --> soft clip --> bus
//
//   sweep: the whole bank moves with the amplitude envelope level (rise into a note, fall out of a hit).
//
// Reused DSP: AdsrEnvelope (amplitude, incl. its retrigger fade), PercNoise (deterministic xorshift),
// PercCore (lifecycle/pan/routing), setTableParam; the tanh drive and soft clip are the same inline forms
// PercSynth uses. New: the pink/brown/metal/crackle/burst generators, the tilt filter, the grain gate and
// the resonator bank (TPT state-variable, retuned at control rate).
//
// Mono per instance; a retrigger of a sounding note is continuous (the amplitude fades out over ~2 ms,
// then re-attacks; the sources, grains and resonators carry); a fresh note resets everything so identical
// inputs render identical samples. Not tempo-synced. No allocation after configure().
namespace webdsp {

inline constexpr InstrumentParamInfo kNoiseParams[] = {
    {static_cast<int32_t>(NoiseParam::Source), "source", 0.0f, 5.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(NoiseParam::Color), "color", 0.0f, 1.0f, 0.6f, "", ModDomain::Linear},
    {static_cast<int32_t>(NoiseParam::Texture), "texture", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(NoiseParam::Density), "density", 2.0f, 4000.0f, 40.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(NoiseParam::Tune), "tune", 40.0f, 12000.0f, 900.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(NoiseParam::Resonance), "resonance", 0.0f, 1.0f, 0.4f, "", ModDomain::Linear},
    {static_cast<int32_t>(NoiseParam::Spread), "spread", 0.0f, 1.0f, 0.45f, "", ModDomain::Linear},
    {static_cast<int32_t>(NoiseParam::Sweep), "sweep", -4.0f, 4.0f, 0.0f, "oct", ModDomain::Linear},
    {static_cast<int32_t>(NoiseParam::Attack), "attack", 0.0005f, 8.0f, 0.001f, "s"},
    {static_cast<int32_t>(NoiseParam::Decay), "decay", 0.02f, 8.0f, 0.25f, "s"},
    {static_cast<int32_t>(NoiseParam::Sustain), "sustain", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(NoiseParam::Release), "release", 0.005f, 8.0f, 0.2f, "s"},
    {static_cast<int32_t>(NoiseParam::Drive), "drive", 0.0f, 1.0f, 0.15f, "", ModDomain::Linear},
    {static_cast<int32_t>(NoiseParam::Tone), "tone", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(NoiseParam::Mix), "mix", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
};
inline constexpr int32_t kNoiseParamCount =
    static_cast<int32_t>(sizeof(kNoiseParams) / sizeof(kNoiseParams[0]));

namespace noisedsp {

inline constexpr double kTwoPi = 6.283185307179586;
inline constexpr int32_t kBands = 6;

inline float flush(float v) { return std::fabs(v) < 1e-20f ? 0.0f : v; }
inline float sqr(float x) { return x * x; }
inline float u01(PercNoise& r) { return 0.5f + 0.5f * r.next(); }

// Power spectral density of a source at `hz`, relative to its own mean over the band (so white = 1): pink falls
// as 1/f (from 20 Hz), brown as 1/(1 + (f/15)^2). Metal, crackle and burst are treated as flat.
inline double sourcePsd(int32_t source, double hz) {
  if (source == 1) return 3385.0 / std::max(hz, 20.0);
  if (source == 2) return 1018.0 / (1.0 + (hz / 15.0) * (hz / 15.0));
  return 1.0;
}

// One knob, dark -> flat -> bright. Two things move together so the whole range stays useful: a
// first-order tilt around 700 Hz (+-12 dB at the ends) and, from the middle outwards, a 12 dB/oct
// low-pass sweeping down (rumble) or high-pass sweeping up (hats), levelled so the ends stay audible.
// Exactly transparent at 0.5. `leveled` (colour, which sits on the source) normalises the gains and corner makeup
// for white input so the tilt moves the spectrum, not the level; the tone stage after the drive does not (it
// cannot know what it is filtering) and is a plain +-6 dB tilt with unity gain at its 700 Hz pivot.
class Tilt {
 public:
  void configure(double sampleRate, bool leveled) { sr_ = sampleRate; leveled_ = leveled; setValue(0.5f); reset(); }
  void reset() { split_ = c1_ = c2_ = 0.0f; }
  void setValue(float v) {
    const float t = std::clamp((v - 0.5f) * 2.0f, -1.0f, 1.0f);
    neutral_ = std::fabs(t) < 1e-4f;
    // 700 Hz is ~3 % of the band, so for white input the high gain dominates the level.
    const float oct = leveled_ ? 2.0f : 1.0f;
    const float lo = std::exp2(-oct * t), hi = std::exp2(oct * t);
    const float norm = leveled_ ? 1.0f / std::sqrt(0.03f * lo * lo + 0.97f * hi * hi) : 1.0f;
    gLow_ = lo * norm;
    gHigh_ = hi * norm;
    aSplit_ = coef(700.0);
    if (t < 0.0f) {
      mode_ = 1;
      const double hz = 16000.0 * std::exp2(static_cast<double>(t) * 7.5);
      a_ = coef(hz);
      comp_ = leveled_ ? std::clamp(static_cast<float>(std::sqrt(16000.0 / hz)) * 0.8f, 1.0f, 8.0f) : 1.0f;
    } else if (t > 0.0f) {
      mode_ = 2;
      a_ = coef(20.0 * std::exp2(static_cast<double>(t) * 8.8));
      comp_ = leveled_ ? 1.0f + 2.9f * t * t * t : 1.0f;
    } else {
      mode_ = 0;
      a_ = 0.0f;
      comp_ = 1.0f;
    }
  }
  float process(float x) {
    if (neutral_) return x;
    split_ += aSplit_ * (x - split_);
    float y = split_ * gLow_ + (x - split_) * gHigh_;
    if (mode_ == 1) {
      c1_ += a_ * (y - c1_);
      c2_ += a_ * (c1_ - c2_);
      y = c2_ * comp_;
    } else if (mode_ == 2) {
      c1_ += a_ * (y - c1_);
      const float h = y - c1_;
      c2_ += a_ * (h - c2_);
      y = (h - c2_) * comp_;
    }
    return y;
  }
  void flushDenormals() { split_ = flush(split_); c1_ = flush(c1_); c2_ = flush(c2_); }

  // Power response at `hz` (exact digital response of the same filters): what the bank needs to know to level
  // itself against what this tilt leaves in the signal.
  double powerAt(double hz) const {
    if (neutral_) return 1.0;
    const std::complex<double> z1 = std::polar(1.0, -kTwoPi * std::min(hz, sr_ * 0.5) / sr_);   // z^-1
    auto lp = [&](double a) { return a / (1.0 - (1.0 - a) * z1); };
    const std::complex<double> l = lp(aSplit_);
    std::complex<double> h = static_cast<double>(gLow_) * l + static_cast<double>(gHigh_) * (1.0 - l);
    if (mode_ == 1) {
      const std::complex<double> c = lp(a_);
      h *= c * c * static_cast<double>(comp_);
    } else if (mode_ == 2) {
      const std::complex<double> c = 1.0 - lp(a_);
      h *= c * c * static_cast<double>(comp_);
    }
    return std::norm(h);
  }

 private:
  float coef(double hz) const {
    return static_cast<float>(1.0 - std::exp(-kTwoPi * std::min(hz, sr_ * 0.45) / sr_));
  }
  double sr_ = 48000.0;
  float split_ = 0, c1_ = 0, c2_ = 0;
  float gLow_ = 1, gHigh_ = 1, aSplit_ = 0.1f, a_ = 0, comp_ = 1;
  int32_t mode_ = 0;
  bool neutral_ = true, leveled_ = true;
};

// Six resonators in parallel (TPT state-variable band-passes, stable under retuning). Each is levelled
// for noise input (output rms ~ independent of Q and frequency) so Resonance changes ring, not loudness.
class Bank {
 public:
  void configure(double sampleRate) { sr_ = sampleRate; reset(); }
  void reset() { for (int32_t b = 0; b < kBands; b++) ic1_[b] = ic2_[b] = 0.0f; }

  // `hz`: the bank root (already pitched/swept); `res`, `spread`: 0..1. `psdAt(f)`: the relative power the
  // exciting signal has at f (1 = white); each band is levelled against it, so a dark or pink source does not
  // make a resonator at its loud end jump in level (and Mix stays a crossfade, not a loudness control).
  template <class Psd>
  void tune(double hz, float res, float spread, Psd&& psdAt) {
    // Ratio anchors: unison (slightly detuned), harmonic, free-bar bell, metallic cluster.
    static constexpr double kAnchors[4][kBands] = {
        {1.0, 1.0041, 0.9959, 1.0082, 0.9918, 1.0021},
        {1.0, 2.0, 3.0, 4.0, 5.0, 6.0},
        {1.0, 2.756, 5.404, 8.933, 13.344, 18.64},
        {1.0, 1.483, 1.800, 2.546, 2.630, 3.897},
    };
    const float pos = std::clamp(spread, 0.0f, 1.0f) * 3.0f;
    const int32_t lo = std::min(static_cast<int32_t>(pos), 2);
    const double frac = static_cast<double>(pos - static_cast<float>(lo));
    const double q0 = 0.7 * std::exp(static_cast<double>(res) * std::log(2000.0 / 0.7));
    double energy = 0.0;
    float w[kBands];
    for (int32_t b = 0; b < kBands; b++) {
      w[b] = 1.0f / (1.0f + 0.45f * static_cast<float>(b));
      energy += static_cast<double>(w[b]) * w[b];
    }
    const float norm = static_cast<float>(1.0 / std::sqrt(energy));
    for (int32_t b = 0; b < kBands; b++) {
      const double ratio = std::exp((1.0 - frac) * std::log(kAnchors[lo][b]) + frac * std::log(kAnchors[lo + 1][b]));
      const double f = hz * ratio;
      // Fade a band out as it nears Nyquist rather than letting it fold or go unstable.
      const double edge = std::clamp((sr_ * 0.46 - f) / (sr_ * 0.06), 0.0, 1.0);
      const double q = std::max(q0 / (1.0 + 0.35 * b), 0.5);
      const double g = std::tan(kTwoPi * 0.5 * std::min(f, sr_ * 0.46) / sr_);
      const double k = 1.0 / q;
      a1_[b] = static_cast<float>(1.0 / (1.0 + g * (g + k)));
      a2_[b] = static_cast<float>(g) * a1_[b];
      a3_[b] = static_cast<float>(g) * a2_[b];
      // Unit-peak band-pass = k * v1; x sqrt(q sr / (pi f)) gives equal output rms for white input.
      const double noiseGain = std::min(std::sqrt(q * sr_ / (3.14159265358979 * std::max(f, 20.0))), 1000.0);
      const double level = std::clamp(1.0 / std::sqrt(std::max(psdAt(f), 1e-6)), 1.0 / 16.0, 3.0);
      out_[b] = static_cast<float>(k * noiseGain * w[b] * norm * edge * level);
      // Feedback enters through the unit-peak path only (loop gain independent of Q).
      in_[b] = static_cast<float>(1.0 / std::max(noiseGain, 1e-3));
    }
  }

  // x: excitation; fb: regenerative feedback (already scaled), entering each band at unit peak gain.
  float process(float x, float fb) {
    float sum = 0.0f;
    for (int32_t b = 0; b < kBands; b++) {
      const float v3 = (x + fb * in_[b]) - ic2_[b];
      const float v1 = a1_[b] * ic1_[b] + a2_[b] * v3;
      const float v2 = ic2_[b] + a2_[b] * ic1_[b] + a3_[b] * v3;
      ic1_[b] = 2.0f * v1 - ic1_[b];
      ic2_[b] = 2.0f * v2 - ic2_[b];
      sum += v1 * out_[b];
    }
    return sum;
  }
  void flushDenormals() { for (int32_t b = 0; b < kBands; b++) { ic1_[b] = flush(ic1_[b]); ic2_[b] = flush(ic2_[b]); } }

 private:
  double sr_ = 48000.0;
  float a1_[kBands] = {}, a2_[kBands] = {}, a3_[kBands] = {}, out_[kBands] = {}, in_[kBands] = {};
  float ic1_[kBands] = {}, ic2_[kBands] = {};
};

}  // namespace noisedsp

class NoiseInstrument final : public Instrument {
 public:
  NoiseInstrument() {
    for (int32_t i = 0; i < kNoiseParamCount; i++) values_[i] = kNoiseParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Noise; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    ampEnv_.configure(sampleRate);
    ampEnv_.reset();
    color_.configure(sampleRate, true);
    tone_.configure(sampleRate, false);
    bank_.configure(sampleRate);
    dcBlockR_ = static_cast<float>(1.0 - noisedsp::kTwoPi * kDcCornerHz / sampleRate);
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    const float oldVelocity = core_.velocity;
    const double oldPitchRatio = core_.pitchRatio;
    if (core_.begin(velocity, delayFrames, pan, pitch)) clearState();  // retrigger carries state
    ampEnv_.noteOnRestart(ampParams());
    if (ampEnv_.stage() == AdsrEnvelope::Stage::Fade) {
      // Retrigger of a sounding note: velocity and pitch wait for the fade to reach silence.
      pendingVelocity_ = core_.velocity;
      pendingPitchRatio_ = core_.pitchRatio;
      core_.velocity = oldVelocity;
      core_.pitchRatio = oldPitchRatio;
      restartPending_ = true;
      return;
    }
    restartPending_ = false;
  }

  void noteOff() override {
    restartPending_ = false;  // released inside the fade: the new note never starts
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    ampEnv_.noteOff(ampParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kNoiseParams, kNoiseParamCount, values_, param, v);
  }

  void reset() override {
    ampEnv_.reset();
    clearState();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    const int32_t fade = restartPending_ ? ampEnv_.fadeFramesLeft() : 0;
    if (core_.active && fade > 0 && fade < numFrames && core_.pendingDelay == 0) {
      renderSegment(busChannels, busChannelCount, fade, scratch);
      core_.velocity = pendingVelocity_;
      core_.pitchRatio = pendingPitchRatio_;
      restartPending_ = false;
      float* shifted[kMaxDspChannels];
      const int32_t channels = std::min(busChannelCount, kMaxDspChannels);
      for (int32_t c = 0; c < channels; c++) shifted[c] = busChannels[c] + fade;
      return renderSegment(shifted, busChannelCount, numFrames - fade, scratch);
    }
    return renderSegment(busChannels, busChannelCount, numFrames, scratch);
  }

  bool renderSegment(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
                     float* const* scratch) {
    using namespace noisedsp;
    if (!core_.active) return false;
    float* mono = scratch[0];
    float* gains = scratch[1];
    const double sr = core_.sampleRate;

    const int32_t source = std::clamp(static_cast<int32_t>(value(NoiseParam::Source) + 0.5f), 0, 5);
    const float density = value(NoiseParam::Density);
    const float tune = value(NoiseParam::Tune);
    const float res = value(NoiseParam::Resonance);
    const float spread = value(NoiseParam::Spread);
    const float sweep = value(NoiseParam::Sweep);
    const float mix = value(NoiseParam::Mix);
    const float velocity = std::min(core_.velocity, 1.0f);

    if (value(NoiseParam::Color) != lastColor_ || source != lastNormSource_) {
      color_.setValue(value(NoiseParam::Color));
      lastColor_ = value(NoiseParam::Color);
      lastNormSource_ = source;
      colorNorm_ = colorLevel(source);
    }
    if (value(NoiseParam::Tone) != lastTone_) { tone_.setValue(value(NoiseParam::Tone)); lastTone_ = value(NoiseParam::Tone); }

    // Texture: depth of the grain gate, and the grain decay (60 ms down to ~0.25 ms).
    const float texture = value(NoiseParam::Texture);
    const float depth = smooth(texture / 0.35f);
    const float grainTau = 0.06f * std::exp2(-7.9f * texture);
    const float grainCoef = percDecayCoef(grainTau, sr);
    const float eventP = static_cast<float>(std::min(static_cast<double>(density) / sr, 0.5));
    // Mean-square of the gate (1 - d + d x env): level back what the gaps took, bounded.
    const float meanEnv = std::min(1.0f, density * grainTau * 0.7f);
    const float meanSq = std::min(1.0f, density * grainTau * 0.25f);
    const float gateMs = sqr(1.0f - depth) + 2.0f * depth * (1.0f - depth) * meanEnv + depth * depth * meanSq;
    const float gateMakeup = 1.0f / std::sqrt(std::max(gateMs, 0.04f));

    // Mix: equal power between the raw noise and the resonators.
    const float dryGain = std::cos(mix * 1.5707964f), wetGain = std::sin(mix * 1.5707964f);

    // Drive: asymmetric tanh, level-compensated (as PercSynth); past 0.25 it also feeds the bank back on
    // itself. The feedback is a soft-saturated copy of the pre-drive mix entering each band at unit peak
    // gain: worst-case small-signal loop gain is 0.5 x 0.8 x ~2.2 < 1, so it lengthens rings and cannot latch.
    const float drive = value(NoiseParam::Drive) * (0.7f + 0.3f * velocity);
    const float g = std::pow(10.0f, kMaxDriveDb * std::pow(drive, 0.8f) * 0.05f);
    const float bias = kDriveBias * drive;
    const float biasTanh = std::tanh(g * bias);
    const float comp = kDrivePeak / std::max(0.5f * (std::tanh(g * (kDrivePeak + bias)) -
                                                     std::tanh(g * (bias - kDrivePeak))), 0.02f);
    const float wet = std::min(drive * 6.0f, 1.0f);
    const float fbAmt = 0.5f * sqr(std::max(0.0f, (drive - 0.25f) / 0.75f));
    const float exc = source == 4 ? kCrackleExcite : 1.0f;

    const float levelGain = core_.velocity * kLevel;

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

      // --- control rate: the bank follows pitch, sweep and its parameters.
      const double hz = static_cast<double>(tune) * core_.pitchRatio *
                        std::exp2(static_cast<double>(sweep) * ampEnv_.level());
      if (std::fabs(hz - lastHz_) > 0.0005 * hz || res != lastRes_ || spread != lastSpread_ ||
          source != lastSource_ || value(NoiseParam::Color) != lastBankColor_) {
        const double n2 = static_cast<double>(colorNorm_) * colorNorm_;
        bank_.tune(hz, res, spread, [&](double f) { return sourcePsd(source, f) * color_.powerAt(f) * n2; });
        lastHz_ = hz; lastRes_ = res; lastSpread_ = spread;
        lastSource_ = source; lastBankColor_ = value(NoiseParam::Color);
      }

      // --- audio rate.
      for (int32_t k = 0; k < n; k++) {
        float s = nextSource(source, density, texture);
        s = color_.process(s) * colorNorm_;

        if (depth > 0.0f) {
          if (noisedsp::u01(event_) < eventP) grain_ = std::max(grain_, 0.4f + 0.6f * noisedsp::u01(event_));
          grain_ *= grainCoef;
          s *= ((1.0f - depth) + depth * grain_) * gateMakeup;
        }

        const float resonated = bank_.process(s * exc, fbAmt * fbState_);
        const float mixed = s * dryGain + resonated * wetGain;
        fbState_ = std::tanh(0.8f * mixed);

        float shaped = mixed;
        if (wet > 0.0f) {
          const float y = (std::tanh(g * (mixed + bias)) - biasTanh) * comp;
          shaped = mixed + (y - mixed) * wet;
        }
        float y = shaped - dcX1_ + dcBlockR_ * dcY1_;
        y = std::clamp(y, -4.0f, 4.0f);
        dcX1_ = shaped;
        dcY1_ = std::fabs(y) < 1e-20f ? 0.0f : y;
        mono[i + k] = tone_.process(y);

        const float e = ampEnv_.next();
        gains[i + k] = e * e * levelGain;
      }
      i += n;
    }

    for (int32_t k = lead; k < i; k++) mono[k] = softClip(mono[k] * gains[k]);
    for (int32_t k = i; k < numFrames; k++) mono[k] = 0.0f;
    bank_.flushDenormals();
    color_.flushDenormals();
    tone_.flushDenormals();
    fbState_ = flush(fbState_); grain_ = flush(grain_); brown_ = flush(brown_); crackle_ = flush(crackle_);

    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr float kLevel = 0.7f;
  static constexpr int32_t kSubBlock = 16;
  static constexpr float kMaxDriveDb = 24.0f;
  // The drive's bias rectifies a click into a DC step; a slow blocker would drag that out as a thump after every
  // tick, so the corner is high enough for it to die in ~9 ms (a 55 Hz drone loses ~0.5 dB).
  static constexpr double kDcCornerHz = 18.0;
  static constexpr float kDriveBias = 0.1f;
  static constexpr float kDrivePeak = 0.6f;
  static constexpr float kCrackleExcite = 6.0f;  // impulses carry little noise power: excite the bank harder

  static int32_t idx(NoiseParam p) { return static_cast<int32_t>(p); }
  float value(NoiseParam p) const { return values_[idx(p)]; }
  static float sqr(float x) { return x * x; }
  static float smooth(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }

  AdsrParams ampParams() const {
    return AdsrParams{value(NoiseParam::Attack), value(NoiseParam::Decay),
                      std::sqrt(value(NoiseParam::Sustain)), value(NoiseParam::Release)};
  }

  // The six sources, each scaled to roughly unit rms so switching source does not change loudness.
  float nextSource(int32_t source, float density, float texture) {
    (void)texture;
    const float w = noise_.next();
    switch (source) {
      case 0:
        return w * kWhiteGain;
      case 1: {  // pink: Kellet's filter, -3 dB/oct
        b0_ = 0.99886f * b0_ + w * 0.0555179f;
        b1_ = 0.99332f * b1_ + w * 0.0750759f;
        b2_ = 0.96900f * b2_ + w * 0.1538520f;
        b3_ = 0.86650f * b3_ + w * 0.3104856f;
        b4_ = 0.55000f * b4_ + w * 0.5329522f;
        b5_ = -0.7616f * b5_ - w * 0.0168980f;
        const float p = b0_ + b1_ + b2_ + b3_ + b4_ + b5_ + b6_ + w * 0.5362f;
        b6_ = w * 0.115926f;
        return p * kPinkGain;
      }
      case 2:  // brown: leaky integrator of white, -6 dB/oct, bounded
        brown_ = 0.998f * brown_ + w * 0.04f;
        return brown_ * kBrownGain;
      case 3: {  // metal: six jittered squares, combined by products (XOR-like) into dense inharmonic buzz
        float sq[6];
        for (int32_t m = 0; m < 6; m++) {
          const double inc = metalInc_[m] * core_.pitchRatio * (1.0 + 0.004 * noise_.next());
          metalPhase_[m] += inc;
          metalPhase_[m] -= std::floor(metalPhase_[m]);
          sq[m] = metalPhase_[m] < 0.5 ? 1.0f : -1.0f;
        }
        return (sq[0] * sq[1] + sq[2] * sq[3] + sq[4] * sq[5] + sq[0] * sq[3] * sq[5] + sq[1] * sq[2] * sq[4]) *
               kMetalGain;
      }
      case 4: {  // crackle: sparse random-signed ticks (rate = density), each a short decaying spike
        const float p = static_cast<float>(std::min(static_cast<double>(density) / core_.sampleRate, 0.5));
        if (noisedsp::u01(event_) < p) {
          const float a = 0.3f + 0.7f * noisedsp::u01(event_);
          crackle_ = event_.next() < 0.0f ? -a : a;
        }
        const float out = crackle_;
        crackle_ *= 0.35f;
        return out;
      }
      default: {  // burst: white noise in jittered bursts (rate = density, ~35 % duty), parabolic window
        if (burstWait_ <= 0) {
          const double period = core_.sampleRate / std::max(static_cast<double>(density), 1.0);
          burstLen_ = std::max(1, static_cast<int32_t>(period * 0.35));
          burstPos_ = 0;
          burstWait_ = std::max(burstLen_ + 1, static_cast<int32_t>(period * (0.65 + 0.7 * noisedsp::u01(event_))));
        }
        burstWait_--;
        if (burstPos_ >= burstLen_) return 0.0f;
        const float x = (static_cast<float>(burstPos_) + 0.5f) / static_cast<float>(burstLen_);
        burstPos_++;
        return w * 4.0f * x * (1.0f - x) * kBurstGain;
      }
    }
  }

  // The gain that leaves the coloured source at the level of the uncoloured one: mean over the band of
  // source PSD x tilt power, on a log grid (weight f). Colour and source both move the level, so this is
  // recomputed when either changes, not at audio rate.
  float colorLevel(int32_t source) const {
    const double nyq = core_.sampleRate * 0.5;
    constexpr int32_t kPoints = 160;
    const double f0 = 10.0, ratio = std::pow(nyq * 0.98 / f0, 1.0 / (kPoints - 1));
    double acc = 0.0, f = f0;
    for (int32_t i = 0; i < kPoints; i++, f *= ratio) acc += noisedsp::sourcePsd(source, f) * color_.powerAt(f) * f;
    const double mean = acc * std::log(ratio) / nyq;
    return static_cast<float>(std::clamp(1.0 / std::sqrt(std::max(mean, 1e-6)), 0.05, 30.0));
  }

  // Transparent below 0.6, then a smooth knee bounded by exactly 1 (as PercSynth).
  static float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.6f) return x;
    const float u = std::min((a - 0.6f) * 2.5f, 3.0f);
    const float y = 0.6f + 0.4f * (u * (27.0f + u * u) / (27.0f + 9.0f * u * u));
    const float c = std::min(y, 1.0f);
    return x < 0.0f ? -c : c;
  }

  void clearState() {
    restartPending_ = false;
    noise_.reset();
    event_.reset();
    event_.s ^= 0x5BD1E995u;
    color_.reset();
    tone_.reset();
    bank_.reset();
    b0_ = b1_ = b2_ = b3_ = b4_ = b5_ = b6_ = brown_ = crackle_ = 0.0f;
    // 808-family inharmonic square set; phases spread so the first samples are not one aligned edge.
    static constexpr double kMetalHz[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
    for (int32_t m = 0; m < 6; m++) {
      metalInc_[m] = kMetalHz[m] / core_.sampleRate;
      metalPhase_[m] = 0.13 * m;
    }
    burstWait_ = 0; burstPos_ = 0; burstLen_ = 0;
    grain_ = 0.0f;
    fbState_ = 0.0f;
    dcX1_ = dcY1_ = 0.0f;
    lastColor_ = lastTone_ = -1.0f; lastNormSource_ = -1; colorNorm_ = 1.0f;
    lastHz_ = -1.0; lastRes_ = lastSpread_ = lastBankColor_ = -1.0f; lastSource_ = -1;
  }

  // Source levels (rms of the raw generator -> ~0.5 of full scale with the post-gain), measured in noise_test.
  static constexpr float kWhiteGain = 1.0f;
  static constexpr float kPinkGain = 0.381f;
  static constexpr float kBrownGain = 2.38f;
  static constexpr float kMetalGain = 0.28f;
  static constexpr float kBurstGain = 2.2f;

  float values_[kNoiseParamCount] = {};
  PercCore core_;
  AdsrEnvelope ampEnv_;
  bool restartPending_ = false;
  float pendingVelocity_ = 0.0f;
  double pendingPitchRatio_ = 1.0;
  PercNoise noise_, event_;
  noisedsp::Tilt color_, tone_;
  noisedsp::Bank bank_;
  float b0_ = 0, b1_ = 0, b2_ = 0, b3_ = 0, b4_ = 0, b5_ = 0, b6_ = 0, brown_ = 0, crackle_ = 0;
  double metalInc_[6] = {}, metalPhase_[6] = {};
  int32_t burstWait_ = 0, burstPos_ = 0, burstLen_ = 0;
  float grain_ = 0.0f, fbState_ = 0.0f;
  float dcX1_ = 0.0f, dcY1_ = 0.0f, dcBlockR_ = 0.999f;
  float lastColor_ = -1.0f, lastTone_ = -1.0f, lastRes_ = -1.0f, lastSpread_ = -1.0f;
  float lastBankColor_ = -1.0f, colorNorm_ = 1.0f;
  int32_t lastSource_ = -1, lastNormSource_ = -1;
  double lastHz_ = -1.0;
};

}  // namespace webdsp
