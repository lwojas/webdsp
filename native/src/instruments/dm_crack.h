#pragma once
#include "digital_machine.h"

// Digital Machine "Crack": FM snare / clap / rim / noisy percussion. Docs: docs/digital-machines.md.
//
//   noise -> band-pass (`tone`) -> nf
//   modulator M (1.59 x f, loopback feedback) --index(t)--> carrier C (f, small pitch drop)
//                              nf --`fm` x depth--> phase of C        <- noise INSIDE the FM path
//   body = C x body env x `body`;   crack = nf x noise env x `noise`  (the noise envelope fires 1-5 times)
//   out = Drive(body + crack)
//
// Noise as a phase modulator makes the body a pitched, hissing tone rather than "tone + filtered
// noise"; at high `fm` the body is mostly noise-shaped sidebands. `burst` re-fires the noise envelope
// (about 7-13 ms apart; every burst but the last decays in ~12 ms, the last carries `decay`), turning
// a snare into a clap, and fades the body out (a clap has none).
namespace webdsp {

inline constexpr InstrumentParamInfo kDmCrackParams[] = {
    {static_cast<int32_t>(DmCrackParam::Tune), "tune", 80.0f, 600.0f, 190.0f, "Hz"},
    {static_cast<int32_t>(DmCrackParam::Decay), "decay", 0.04f, 1.0f, 0.18f, "s"},
    {static_cast<int32_t>(DmCrackParam::Fm), "fm", 0.0f, 1.0f, 0.4f, ""},
    {static_cast<int32_t>(DmCrackParam::Body), "body", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(DmCrackParam::Noise), "noise", 0.0f, 1.0f, 0.6f, ""},
    {static_cast<int32_t>(DmCrackParam::Burst), "burst", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(DmCrackParam::Tone), "tone", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(DmCrackParam::Drive), "drive", 0.0f, 1.0f, 0.2f, ""},
};
inline constexpr int32_t kDmCrackParamCount =
    static_cast<int32_t>(sizeof(kDmCrackParams) / sizeof(kDmCrackParams[0]));

class DmCrack {
 public:
  static constexpr InstrumentKind kKind = InstrumentKind::DmCrack;
  static constexpr const InstrumentParamInfo* kParams = kDmCrackParams;
  static constexpr int32_t kParamCount = kDmCrackParamCount;

  // Number of noise bursts for a `burst` value (1..5); exposed for tests.
  static int32_t burstCount(float burst) { return 1 + static_cast<int32_t>(std::lround(std::clamp(burst, 0.0f, 1.0f) * 4.0f)); }

  void configure(double sampleRate, const float* table) {
    sr_ = sampleRate;
    table_ = table;
    body_.configure(sampleRate);
    noiseEnv_.configure(sampleRate);
    index_.configure(sampleRate);
    pitch_.configure(sampleRate);
    reset();
  }

  void noteOn(const float* v, float velocity, double pitchRatio, bool fresh) {
    vel_ = velocity;
    pitchRatio_ = pitchRatio;
    if (fresh) clearState();
    const float decay = v[idx(DmCrackParam::Decay)];
    const float burst = v[idx(DmCrackParam::Burst)];
    const int32_t count = burstCount(burst);
    decayCoef_ = percDecayCoef(decay * kDecayToTau, sr_);
    shortCoef_ = percDecayCoef(0.012f * kDecayToTau, sr_);
    body_.trigger(0.0004f, percDecayCoef(decay * 0.7f * kDecayToTau, sr_), 0.0f);
    index_.trigger(0.0002f, percDecayCoef(decay * 0.3f * kDecayToTau, sr_), 0.0f);
    pitch_.start(1.0f, percDecayCoef(0.03f * kDecayToTau, sr_), 0.0f);
    noiseEnv_.trigger(0.0002f, count > 1 ? shortCoef_ : decayCoef_, 0.0f);
    burstsLeft_ = count - 1;
    spacing_ = std::max<int32_t>(1, static_cast<int32_t>((0.007 + 0.006 * burst) * sr_));
    toNext_ = spacing_;
    noise_.reset();
    releaseCoef_ = percDecayCoef(0.01f * kDecayToTau, sr_);
  }

  void noteOff(const float*) {
    burstsLeft_ = 0;
    body_.release(releaseCoef_);
    noiseEnv_.release(releaseCoef_);
    index_.release(releaseCoef_);
  }

  void reset() {
    body_.reset(); noiseEnv_.reset(); index_.reset(); pitch_.reset();
    burstsLeft_ = 0;
    clearState();
  }

  bool render(const float* v, float* out, int32_t n) {
    const float tune = v[idx(DmCrackParam::Tune)];
    const float fm = v[idx(DmCrackParam::Fm)];
    const float bodyAmt = v[idx(DmCrackParam::Body)];
    const float noiseAmt = v[idx(DmCrackParam::Noise)];
    const float burst = v[idx(DmCrackParam::Burst)];
    const float tone = v[idx(DmCrackParam::Tone)];
    const float vf = fmk::velocityBrightness(vel_);
    drive_.set(v[idx(DmCrackParam::Drive)]);
    bp_.set(1200.0 * std::exp2(2.5 * static_cast<double>(tone)), 0.9, sr_);

    // --- control rate.
    const double f = std::clamp(static_cast<double>(tune) * pitchRatio_ * std::exp2(kDrop * pitch_.level()),
                                20.0, std::min(4000.0, sr_ * 0.2));
    const double incC = f / sr_;
    const double incTarget = incC * kModRatio;
    const float idxMax = fmk::maxIndex(f * kModRatio * 1.9, sr_);
    const float bodyGain = (0.2f + 0.8f * bodyAmt) * (1.0f - 0.85f * burst);
    if (incC_ < 0.0) { incC_ = incC; incM_ = incTarget; }
    const double stepC = (incC - incC_) / n;
    const double stepM = (incTarget - incM_) / n;

    for (int32_t k = 0; k < n; k++) {
      incC_ += stepC;
      incM_ += stepM;
      c_.advance(incC_);
      m_.advance(incM_);

      if (burstsLeft_ > 0 && --toNext_ <= 0) {
        burstsLeft_--;
        noiseEnv_.trigger(0.0002f, burstsLeft_ > 0 ? shortCoef_ : decayCoef_, 0.0f);
        toNext_ = spacing_;
      }
      const float e = body_.next();
      const float ie = index_.next();
      const float ne = noiseEnv_.next();
      pitch_.next();

      float lp, bp, hp;
      bp_.process(noise_.next(), lp, bp, hp);
      const float nf = bp * kNoiseGain;

      const float idx = std::min(vf * fm * kIndexScale * (0.25f + 0.75f * ie), idxMax);
      const float fb = kFeedback * std::min(idx * 0.2f, 1.0f) * 0.5f * (fb1_ + fb2_);
      const float mod = m_.at(table_, fb);
      fb2_ = fb1_;
      fb1_ = mod;
      // Noise phase-modulation: strongest at the strike, never fully gone while the body rings.
      const float nPm = fm * kNoisePm * (0.3f + 0.7f * ie) * nf;
      const float c = c_.at(table_, idx * mod + nPm);

      const float s = c * e * bodyGain + nf * ne * noiseAmt;
      out[k] = drive_.process(s) * vel_ * kLevel;
    }
    incC_ = incC;
    incM_ = incTarget;
    return !(body_.idle() && noiseEnv_.idle() && burstsLeft_ == 0);
  }

 private:
  static constexpr float kLevel = 0.6f;
  static constexpr float kIndexScale = 4.0f;  // radians at fm 1
  static constexpr float kNoisePm = 2.5f;     // radians of noise phase-modulation at fm 1
  static constexpr float kNoiseGain = 2.2f;   // the band-pass takes most of white noise's level
  static constexpr float kFeedback = 0.9f;
  static constexpr double kModRatio = 1.59;   // first mode of an ideal circular membrane
  static constexpr double kDrop = 0.6;        // octaves of pitch drop at the strike

  static int32_t idx(DmCrackParam p) { return static_cast<int32_t>(p); }

  void clearState() {
    c_.phase = 0.25;
    m_.phase = 0.0;
    fb1_ = fb2_ = 0.0f;
    incC_ = incM_ = -1.0;
    noise_.reset();
    bp_.reset();
  }

  double sr_ = 48000.0;
  const float* table_ = nullptr;
  float vel_ = 1.0f;
  double pitchRatio_ = 1.0;
  fmk::ExpEnv body_, noiseEnv_, index_, pitch_;
  fmk::FmOp c_, m_;
  float fb1_ = 0.0f, fb2_ = 0.0f;
  double incC_ = -1.0, incM_ = -1.0;
  PercNoise noise_;
  fmk::Svf bp_;
  fmk::Drive drive_;
  float decayCoef_ = 0.99f, shortCoef_ = 0.99f, releaseCoef_ = 0.99f;
  int32_t burstsLeft_ = 0, spacing_ = 480, toNext_ = 0;
};

using DmCrackInstrument = DigitalMachine<DmCrack>;

}  // namespace webdsp
