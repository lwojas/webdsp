#pragma once
#include "digital_machine.h"

// Digital Machine "Metal": FM hats / cymbals / clangs / bars and bells. Docs: docs/digital-machines.md.
//
//   four operators at a curated inharmonic ratio set (`spread` glides: harmonic -> bar -> 808 -> scattered)
//     B (feedback) --I--> A      D (feedback) --I--> C       two parallel pairs, each also
//     A also hears 0.5 I x D,    C also hears 0.5 I x B      cross-modulated by the other
//     out = 0.4 (A + C) + 0.1 (B + D)  (+ noise: added, and as phase modulation on A)
//   x (body env + `chick` x fast env)  ->  state-variable high-pass (`tone`)  ->  Drive
//
// The index follows its own envelope (half the body decay) and the chick adds a burst, so the hit is
// brighter at the strike. With `noise` 0, a high-pass of 0 and a long decay this is a pure inharmonic
// bar or bell; with noise and the high-pass up it is a hat. The index is limited from the highest
// modulator frequency, and an operator above ~0.4 fs fades out instead of aliasing.
namespace webdsp {

inline constexpr InstrumentParamInfo kDmMetalParams[] = {
    {static_cast<int32_t>(DmMetalParam::Tune), "tune", 80.0f, 1500.0f, 320.0f, "Hz"},
    {static_cast<int32_t>(DmMetalParam::Decay), "decay", 0.02f, 2.0f, 0.12f, "s"},
    {static_cast<int32_t>(DmMetalParam::Fm), "fm", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(DmMetalParam::Spread), "spread", 0.0f, 1.0f, 0.67f, ""},
    {static_cast<int32_t>(DmMetalParam::Tone), "tone", 0.0f, 1.0f, 0.6f, ""},
    {static_cast<int32_t>(DmMetalParam::Noise), "noise", 0.0f, 1.0f, 0.3f, ""},
    {static_cast<int32_t>(DmMetalParam::Chick), "chick", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(DmMetalParam::Drive), "drive", 0.0f, 1.0f, 0.15f, ""},
};
inline constexpr int32_t kDmMetalParamCount =
    static_cast<int32_t>(sizeof(kDmMetalParams) / sizeof(kDmMetalParams[0]));

class DmMetal {
 public:
  static constexpr InstrumentKind kKind = InstrumentKind::DmMetal;
  static constexpr const InstrumentParamInfo* kParams = kDmMetalParams;
  static constexpr int32_t kParamCount = kDmMetalParamCount;

  void configure(double sampleRate, const float* table) {
    sr_ = sampleRate;
    table_ = table;
    body_.configure(sampleRate);
    chick_.configure(sampleRate);
    index_.configure(sampleRate);
    reset();
  }

  void noteOn(const float* v, float velocity, double pitchRatio, bool fresh) {
    vel_ = velocity;
    pitchRatio_ = pitchRatio;
    if (fresh) clearState();
    const float decay = v[idx(DmMetalParam::Decay)];
    body_.trigger(0.0003f, percDecayCoef(decay * kDecayToTau, sr_), 0.0f);
    index_.trigger(0.0002f, percDecayCoef(decay * 0.5f * kDecayToTau, sr_), 0.0f);
    chick_.trigger(0.0002f, percDecayCoef(0.006f * kDecayToTau, sr_), 0.0f);
    noise_.reset();
  }

  void noteOff(const float*) {
    const float r = percDecayCoef(0.004f * kDecayToTau, sr_);  // a short hat chokes an open one
    body_.release(r);
    chick_.release(r);
    index_.release(r);
  }

  void reset() {
    body_.reset(); chick_.reset(); index_.reset();
    clearState();
  }

  bool render(const float* v, float* out, int32_t n) {
    const float tune = v[idx(DmMetalParam::Tune)];
    const float fm = v[idx(DmMetalParam::Fm)];
    const float tone = v[idx(DmMetalParam::Tone)];
    const float noiseAmt = v[idx(DmMetalParam::Noise)];
    const float chickAmt = v[idx(DmMetalParam::Chick)];
    const float vf = fmk::velocityBrightness(vel_);
    drive_.set(v[idx(DmMetalParam::Drive)]);

    // --- control rate: the ratio set for this `spread`, operator frequencies, guards, filter.
    const fmk::MagneticPos m = fmk::magneticPos(v[idx(DmMetalParam::Spread)], kSetCount);
    const double f = std::clamp(static_cast<double>(tune) * pitchRatio_, 20.0, 8000.0);
    double hz[4];
    float gate[4];
    double maxMod = 0.0;
    for (int32_t k = 0; k < 4; k++) {
      const double r = static_cast<double>(kSets[m.i][k]) *
                       std::pow(static_cast<double>(kSets[m.i + 1][k] / kSets[m.i][k]), static_cast<double>(m.t));
      hz[k] = f * r;
      gate[k] = fmk::nyquistGain(hz[k], sr_);
      maxMod = std::max(maxMod, hz[k]);
    }
    const float idxMax = fmk::maxIndex(maxMod * 1.8, sr_);  // x (1 + feedback) of the modulator itself
    const double incA = hz[0] / sr_, incB = hz[1] / sr_, incC = hz[2] / sr_, incD = hz[3] / sr_;
    hp_.set(150.0 * std::exp2(5.3 * static_cast<double>(tone)), 0.8, sr_);

    for (int32_t k = 0; k < n; k++) {
      a_.advance(incA); b_.advance(incB); c_.advance(incC); d_.advance(incD);
      const float e = body_.next();
      const float ck = chick_.next();
      const float ie = index_.next();
      const float amp = e + chickAmt * ck * 0.9f;
      const float I = std::min(vf * fm * kIndexScale * (0.3f + 0.7f * ie + 1.2f * chickAmt * ck), idxMax);
      const float beta = 0.8f * std::min(I * 0.25f, 1.0f);
      const float nz = noise_.next();

      const float bs = b_.at(table_, beta * b1_);
      const float ds = d_.at(table_, beta * d1_);
      b1_ = bs;
      d1_ = ds;
      const float as = a_.at(table_, I * (bs + 0.5f * ds) + noiseAmt * kNoisePm * nz);
      const float cs = c_.at(table_, I * (ds + 0.5f * bs));
      const float mix = 0.4f * (as * gate[0] + cs * gate[2]) + 0.1f * (bs * gate[1] + ds * gate[3]);

      float lp, bp, hp;
      hp_.process((mix + noiseAmt * kNoiseAdd * nz) * amp, lp, bp, hp);
      out[k] = drive_.process(hp) * vel_ * kLevel;
    }
    return !body_.idle();
  }

 private:
  static constexpr float kLevel = 0.8f;
  static constexpr float kIndexScale = 7.0f;  // radians at fm 1
  static constexpr float kNoisePm = 1.5f;     // radians of noise phase-modulation at noise 1
  static constexpr float kNoiseAdd = 0.5f;
  // Ratio sets (operators A, B, C, D): harmonic, free-free bar, 808-style cluster, scattered.
  static constexpr int32_t kSetCount = 4;
  static constexpr float kSets[kSetCount][4] = {
      {1.0f, 2.0f, 3.0f, 4.0f},
      {1.0f, 2.76f, 5.40f, 8.93f},
      {1.0f, 1.483f, 1.800f, 2.546f},
      {1.0f, 2.236f, 3.606f, 5.385f},
  };

  static int32_t idx(DmMetalParam p) { return static_cast<int32_t>(p); }

  void clearState() {
    a_.phase = b_.phase = c_.phase = d_.phase = 0.0;
    b1_ = d1_ = 0.0f;
    noise_.reset();
    hp_.reset();
  }

  double sr_ = 48000.0;
  const float* table_ = nullptr;
  float vel_ = 1.0f;
  double pitchRatio_ = 1.0;
  fmk::ExpEnv body_, chick_, index_;
  fmk::FmOp a_, b_, c_, d_;
  float b1_ = 0.0f, d1_ = 0.0f;
  PercNoise noise_;
  fmk::Svf hp_;
  fmk::Drive drive_;
};

using DmMetalInstrument = DigitalMachine<DmMetal>;

}  // namespace webdsp
