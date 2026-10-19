#pragma once
#include "digital_machine.h"

// Digital Machine "Chaos": a three-operator feedback ring for unstable, strange digital sounds and
// aggressive metallic leads. Docs: docs/digital-machines.md.
//
//   A --G--> B --G--> C --G--> (one sample later) --> A        (+ noise into A's phase)
//   y = 0.5 A + 0.3 B + 0.2 C  ->  wavefold (`fold`)  ->  DC block  x amp env
//
// Every operator is a sine, so the output is bounded by construction no matter how large the loop
// gain G gets (G = chaos^2 x 9 rad, riding a decaying envelope); "chaos" only changes how far the
// spectrum scatters. Low chaos is a clean, slightly inharmonic tone; high chaos is dense and noisy.
// The ring is deterministic: a fresh note clears all state, so identical inputs render identically.
// Unlike the other Machines it does NOT limit the index to avoid aliasing: the folded-over
// brightness is the point (docs/digital-machines.md, "Aliasing policy"). Operators above ~0.4 fs
// still fade out, so a high note stays a note.
namespace webdsp {

inline constexpr InstrumentParamInfo kDmChaosParams[] = {
    {static_cast<int32_t>(DmChaosParam::Tune), "tune", 27.5f, 1760.0f, 220.0f, "Hz"},
    {static_cast<int32_t>(DmChaosParam::Decay), "decay", 0.03f, 3.0f, 0.4f, "s"},
    {static_cast<int32_t>(DmChaosParam::Chaos), "chaos", 0.0f, 1.0f, 0.4f, ""},
    {static_cast<int32_t>(DmChaosParam::Ratio), "ratio", 0.0f, 1.0f, 0.143f, ""},
    {static_cast<int32_t>(DmChaosParam::Fold), "fold", 0.0f, 1.0f, 0.3f, ""},
    {static_cast<int32_t>(DmChaosParam::Noise), "noise", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(DmChaosParam::Sweep), "sweep", 0.0f, 3.0f, 0.0f, "oct"},
    {static_cast<int32_t>(DmChaosParam::Hold), "hold", 0.0f, 1.0f, 0.0f, ""},
};
inline constexpr int32_t kDmChaosParamCount =
    static_cast<int32_t>(sizeof(kDmChaosParams) / sizeof(kDmChaosParams[0]));

class DmChaos {
 public:
  static constexpr InstrumentKind kKind = InstrumentKind::DmChaos;
  static constexpr const InstrumentParamInfo* kParams = kDmChaosParams;
  static constexpr int32_t kParamCount = kDmChaosParamCount;

  void configure(double sampleRate, const float* table) {
    sr_ = sampleRate;
    table_ = table;
    amp_.configure(sampleRate);
    index_.configure(sampleRate);
    pitch_.configure(sampleRate);
    reset();
  }

  void noteOn(const float* v, float velocity, double pitchRatio, bool fresh) {
    vel_ = velocity;
    pitchRatio_ = pitchRatio;
    if (fresh) clearState();
    const float decay = v[idx(DmChaosParam::Decay)];
    const float hold = v[idx(DmChaosParam::Hold)];
    amp_.trigger(0.001f, percDecayCoef(decay * kDecayToTau, sr_), hold);
    index_.trigger(0.0005f, percDecayCoef(decay * 0.5f * kDecayToTau, sr_), hold * 0.6f);
    pitch_.start(1.0f, percDecayCoef(std::clamp(decay * 0.12f, 0.012f, 0.25f) * kDecayToTau, sr_), 0.0f);
    releaseCoef_ = percDecayCoef((0.012f + 0.2f * hold) * kDecayToTau, sr_);
    noise_.reset();
  }

  void noteOff(const float*) {
    amp_.release(releaseCoef_);
    index_.release(releaseCoef_);
  }

  void reset() {
    amp_.reset(); index_.reset(); pitch_.reset();
    clearState();
  }

  bool render(const float* v, float* out, int32_t n) {
    const float tune = v[idx(DmChaosParam::Tune)];
    const float chaos = v[idx(DmChaosParam::Chaos)];
    const float ratio = fmk::magneticRatio(kRatios, v[idx(DmChaosParam::Ratio)]);
    const float fold = v[idx(DmChaosParam::Fold)];
    const float noiseAmt = v[idx(DmChaosParam::Noise)];
    const float sweep = v[idx(DmChaosParam::Sweep)];
    const float vf = fmk::velocityBrightness(vel_);

    // --- control rate.
    const double f = std::clamp(static_cast<double>(tune) * pitchRatio_ * std::exp2(static_cast<double>(sweep) * pitch_.level()),
                                20.0, std::min(6000.0, sr_ * 0.2));
    const double hzA = f, hzB = f * ratio, hzC = f * ratio * kGolden;
    const double incA = hzA / sr_, incB = hzB / sr_, incC = hzC / sr_;
    const float gB = fmk::nyquistGain(hzB, sr_), gC = fmk::nyquistGain(hzC, sr_);
    const float loop = vf * chaos * chaos * kLoopGain;
    const float foldGain = (1.0f + 4.0f * fold) * 0.25f;  // in turns: 1/4 turn maps full scale to full scale
    const float foldMix = std::min(1.0f, fold * 2.0f);

    for (int32_t k = 0; k < n; k++) {
      a_.advance(incA);
      b_.advance(incB);
      c_.advance(incC);
      const float e = amp_.next();
      const float ie = index_.next();
      pitch_.next();

      const float g = loop * (0.4f + 0.6f * ie);
      const float nz = noise_.next();
      const float a = a_.at(table_, g * c1_ + noiseAmt * kNoisePm * nz);
      const float b = b_.at(table_, g * a) * gB;
      const float c = c_.at(table_, g * b) * gC;
      c1_ = c;

      float y = 0.5f * a + 0.3f * b + 0.2f * c;
      if (foldMix > 0.0f) {
        const float folded = fmk::sinTurns(table_, static_cast<double>(y * foldGain));
        y += (folded - y) * foldMix;
      }
      // One-pole DC blocker (a folded, driven ring is not zero-mean).
      const float blocked = y - dcX_ + 0.995f * dcY_;
      dcX_ = y;
      dcY_ = blocked;
      out[k] = blocked * e * vel_ * kLevel;
    }
    return !amp_.idle();
  }

 private:
  static constexpr float kLevel = 0.5f;
  static constexpr float kLoopGain = 9.0f;  // radians of loop gain at chaos 1
  static constexpr float kNoisePm = 2.0f;
  static constexpr double kGolden = 1.6180339887;
  // Ring ratios: sub-octave, 1, sqrt 2, 2, 3, 3.5, 5, 7.07 (sqrt 50)
  static constexpr float kRatios[] = {0.5f, 1.0f, 1.41421356f, 2.0f, 3.0f, 3.5f, 5.0f, 7.0710678f};

  static int32_t idx(DmChaosParam p) { return static_cast<int32_t>(p); }

  void clearState() {
    a_.phase = 0.0; b_.phase = 0.0; c_.phase = 0.0;
    c1_ = 0.0f;
    dcX_ = dcY_ = 0.0f;
    noise_.reset();
  }

  double sr_ = 48000.0;
  const float* table_ = nullptr;
  float vel_ = 1.0f;
  double pitchRatio_ = 1.0;
  fmk::ExpEnv amp_, index_, pitch_;
  fmk::FmOp a_, b_, c_;
  float c1_ = 0.0f, dcX_ = 0.0f, dcY_ = 0.0f;
  float releaseCoef_ = 0.99f;
  PercNoise noise_;
};

using DmChaosInstrument = DigitalMachine<DmChaos>;

}  // namespace webdsp
