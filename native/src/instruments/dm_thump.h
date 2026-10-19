#pragma once
#include "digital_machine.h"

// Digital Machine "Thump": FM kick / tom / tuned percussion / bass hit. Docs: docs/digital-machines.md.
//
//   pitch env (exp, `sweep` octaves) -> f
//   modulator M (ratio x f, loopback feedback `body`) --index(t)--> phase of carrier C (f)
//                                        `--0.5 index--> phase of overtone D (1.59 f, membrane mode)
//   y = C + body-share x D;   x amp env;   + click (band-passed noise, `punch`);   -> Drive
//
// The index follows its own faster envelope (`punch` shortens it and adds a burst), so the hit is
// bright for a few ms and settles to a clean sine. The modulator frequency always follows the live
// (swept) carrier, so the spectrum keeps its shape through the sweep, and its index is limited so
// the sidebands stay below 0.4 fs. The overtone ratio 1.59 is the first mode of an ideal circular
// membrane.
namespace webdsp {

inline constexpr InstrumentParamInfo kDmThumpParams[] = {
    {static_cast<int32_t>(DmThumpParam::Tune), "tune", 25.0f, 400.0f, 55.0f, "Hz"},
    {static_cast<int32_t>(DmThumpParam::Decay), "decay", 0.05f, 2.0f, 0.45f, "s"},
    {static_cast<int32_t>(DmThumpParam::Fm), "fm", 0.0f, 1.0f, 0.35f, ""},
    {static_cast<int32_t>(DmThumpParam::Sweep), "sweep", 0.0f, 4.0f, 2.0f, "oct"},
    {static_cast<int32_t>(DmThumpParam::Punch), "punch", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(DmThumpParam::Ratio), "ratio", 0.0f, 1.0f, 0.17f, ""},
    {static_cast<int32_t>(DmThumpParam::Body), "body", 0.0f, 1.0f, 0.3f, ""},
    {static_cast<int32_t>(DmThumpParam::Drive), "drive", 0.0f, 1.0f, 0.2f, ""},
};
inline constexpr int32_t kDmThumpParamCount =
    static_cast<int32_t>(sizeof(kDmThumpParams) / sizeof(kDmThumpParams[0]));

class DmThump {
 public:
  static constexpr InstrumentKind kKind = InstrumentKind::DmThump;
  static constexpr const InstrumentParamInfo* kParams = kDmThumpParams;
  static constexpr int32_t kParamCount = kDmThumpParamCount;

  void configure(double sampleRate, const float* table) {
    sr_ = sampleRate;
    table_ = table;
    amp_.configure(sampleRate);
    pitch_.configure(sampleRate);
    index_.configure(sampleRate);
    click_.configure(sampleRate);
    reset();
  }

  void noteOn(const float* v, float velocity, double pitchRatio, bool fresh) {
    vel_ = velocity;
    pitchRatio_ = pitchRatio;
    if (fresh) clearState();
    const float decay = v[idx(DmThumpParam::Decay)];
    const float punch = v[idx(DmThumpParam::Punch)];
    amp_.trigger(0.0005f, percDecayCoef(decay * kDecayToTau, sr_), 0.0f);
    pitch_.start(1.0f, percDecayCoef(std::clamp(decay * 0.10f, 0.012f, 0.2f) * kDecayToTau, sr_), 0.0f);
    index_.trigger(0.0001f, percDecayCoef(decay * (0.35f - 0.27f * punch) * kDecayToTau, sr_), 0.0f);
    click_.trigger(0.0001f, percDecayCoef(0.006f * kDecayToTau, sr_), 0.0f);
    noise_.reset();
  }

  void noteOff(const float*) {
    const float r = percDecayCoef(0.012f * kDecayToTau, sr_);
    amp_.release(r);
    index_.release(r);
    click_.release(r);
  }

  void reset() {
    amp_.reset(); pitch_.reset(); index_.reset(); click_.reset();
    clearState();
  }

  bool render(const float* v, float* out, int32_t n) {
    const float tune = v[idx(DmThumpParam::Tune)];
    const float sweep = v[idx(DmThumpParam::Sweep)];
    const float punch = v[idx(DmThumpParam::Punch)];
    const float fm = v[idx(DmThumpParam::Fm)];
    const float body = v[idx(DmThumpParam::Body)];
    const float ratio = fmk::magneticRatio(kRatios, v[idx(DmThumpParam::Ratio)]);
    const float vf = fmk::velocityBrightness(vel_);
    drive_.set(v[idx(DmThumpParam::Drive)]);

    // --- control rate.
    const double f = std::clamp(static_cast<double>(tune) * pitchRatio_ * std::exp2(static_cast<double>(sweep) * pitch_.level()),
                                20.0, std::min(4000.0, sr_ * 0.2));
    const double incC = f / sr_;
    const double targetM = incC * ratio;
    const float idxMax = fmk::maxIndex(f * ratio * 1.9, sr_);  // x (1 + feedback) of the modulator itself
    const float gD = fmk::nyquistGain(f * kOvertone, sr_);
    if (incC_ < 0.0) { incC_ = incC; incM_ = targetM; }
    const double stepC = (incC - incC_) / n;
    const double stepM = (targetM - incM_) / n;
    const float beta = 0.9f * body;

    for (int32_t k = 0; k < n; k++) {
      incC_ += stepC;
      incM_ += stepM;
      c_.advance(incC_);
      m_.advance(incM_);
      d_.advance(incC_ * kOvertone);

      const float e = amp_.next();
      const float ie = index_.next();
      pitch_.next();
      const float ce = click_.next();

      const float idx = std::min(vf * (fm * kIndexScale * (0.2f + 0.8f * ie) + punch * kPunchIndex * ie), idxMax);
      // Feedback grows with the index (0 at index 0, so zero index is a pure sine); two-sample
      // average for stability.
      const float fb = beta * std::min(idx * 0.2f, 1.0f) * 0.5f * (fb1_ + fb2_);
      const float mod = m_.at(table_, fb);
      fb2_ = fb1_;
      fb1_ = mod;
      const float c = c_.at(table_, idx * mod);
      const float dAmp = 0.5f * body * e;
      const float dy = dAmp > 1e-4f ? d_.at(table_, 0.5f * idx * mod) * gD : 0.0f;
      const float y = (c + dAmp * dy) * (1.0f / (1.0f + 0.5f * body));

      const float click = bp_.process(noise_.next()) * ce * punch * kClick;
      out[k] = drive_.process(y * e + click) * vel_ * kLevel;
    }
    incC_ = incC;
    incM_ = targetM;
    return !amp_.idle();
  }

 private:
  static constexpr float kLevel = 0.5f;
  static constexpr float kIndexScale = 5.0f;   // radians of peak index at fm 1
  static constexpr float kPunchIndex = 3.0f;   // extra radians at the strike at punch 1
  static constexpr float kClick = 0.6f;
  static constexpr double kOvertone = 1.59;
  // Modulator ratios: 0.5, 1, membrane overtone, octave, bar partial, 3.5, 5.4
  static constexpr float kRatios[] = {0.5f, 1.0f, 1.59f, 2.0f, 2.76f, 3.5f, 5.4f};

  static int32_t idx(DmThumpParam p) { return static_cast<int32_t>(p); }

  void clearState() {
    c_.phase = 0.25;  // carrier peak: punch from the first cycle (the 0.5 ms attack keeps it click-free)
    m_.phase = 0.0;
    d_.phase = 0.0;
    fb1_ = fb2_ = 0.0f;
    incC_ = incM_ = -1.0;
    noise_.reset();
    bp_.set(2500.0, 0.7, sr_);
    bp_.reset();
  }

  double sr_ = 48000.0;
  const float* table_ = nullptr;
  float vel_ = 1.0f;
  double pitchRatio_ = 1.0;
  fmk::ExpEnv amp_, pitch_, index_, click_;
  fmk::FmOp c_, m_, d_;
  float fb1_ = 0.0f, fb2_ = 0.0f;
  double incC_ = -1.0, incM_ = -1.0;
  PercNoise noise_;
  PercBandpass bp_;
  fmk::Drive drive_;
};

using DmThumpInstrument = DigitalMachine<DmThump>;

}  // namespace webdsp
