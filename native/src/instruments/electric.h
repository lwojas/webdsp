#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/lfo.h"
#include "../dsp/matrix_filter.h"
#include "../dsp/panner.h"
#include "../dsp/percussion.h"

// Electric: a digital electric-keyboard voice. Shipped behaviour and parameter mapping:
// docs/electric-instrument.md. Not a Rhodes / Wurlitzer / DX emulation and not a general FM synth: one fixed,
// purpose-built path and eight musical macros (Body, Strike, Bell, Warmth, Age, Crush, Cutoff, Resonance) plus
// Decay.
//
// per note (a "slot"; kSlots of them so a new note never cuts the previous one's tail):
//   body:  sine + 2nd partial (+ sub) at f0, the fundamental phase-modulated by a ratio-1 "tine" operator
//          whose index is struck high and falls fast (the bark)            x decay curve
//          a second, slightly detuned sine pair (A/B) leaning to opposite channels  -> width + oscillator beat
//   bell:  inharmonic 4 : 5.6 phase-modulation pair (+ a short ring partial), own faster-falling envelope
//   strike: band-passed noise tick + the tine index burst + a faster attack, scaled by velocity
//   sum -> crush (sample-and-hold + quantise, the modulators are quantised too) -> warmth (biased tanh, bell
//          softened) -> ZDF ladder low-pass (per channel; cutoff key-tracked, opened by the strike)
//          -> x gate^2 x velocity x loudness trim
//   slots summed -> soft clip (bounded +-1) -> balance Panner -> bus
//
// Reused: AdsrEnvelope (the gate: attack, release, restart fade), MatrixFilter (ladder), PercNoise/PercBandpass
// (the tick), Panner, setTableParam, the LFO sample-and-hold hash (per-note variation). Everything is a pure
// function of the inputs: reset() restarts the per-note variation sequence, so identical inputs render
// identical samples.
namespace webdsp {

inline constexpr InstrumentParamInfo kElectricParams[] = {
    {static_cast<int32_t>(ElectricParam::Body), "body", 0.0f, 1.0f, 0.55f, "", ModDomain::Linear},
    {static_cast<int32_t>(ElectricParam::Strike), "strike", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(ElectricParam::Bell), "bell", 0.0f, 1.0f, 0.4f, "", ModDomain::Linear},
    {static_cast<int32_t>(ElectricParam::Warmth), "warmth", 0.0f, 1.0f, 0.4f, "", ModDomain::Linear},
    {static_cast<int32_t>(ElectricParam::Age), "age", 0.0f, 1.0f, 0.25f, "", ModDomain::Linear},
    {static_cast<int32_t>(ElectricParam::Crush), "crush", 0.0f, 1.0f, 0.1f, "", ModDomain::Linear},
    {static_cast<int32_t>(ElectricParam::Cutoff), "cutoff", 80.0f, 18000.0f, 7000.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(ElectricParam::Resonance), "resonance", 0.0f, 1.0f, 0.15f, "", ModDomain::Linear},
    {static_cast<int32_t>(ElectricParam::Decay), "decay", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(ElectricParam::Tune), "tune", 32.7f, 1046.5f, 261.63f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(ElectricParam::Release), "release", 0.02f, 4.0f, 0.35f, "s"},
    {static_cast<int32_t>(ElectricParam::Width), "width", 0.0f, 1.0f, 0.6f, "", ModDomain::Linear},
};
inline constexpr int32_t kElectricParamCount =
    static_cast<int32_t>(sizeof(kElectricParams) / sizeof(kElectricParams[0]));

namespace electric {

inline constexpr double kTwoPi = 6.283185307179586;
inline constexpr float kInvTwoPi = 0.15915494309189535f;
inline constexpr int32_t kSlots = 3;
inline constexpr int32_t kSubBlock = 16;  // control-rate step (~0.33 ms at 48 kHz)

// Sine of a phase in cycles (any finite value, any range). Taylor to t^9 on [-pi/2, pi/2]: error < 4e-6.
inline float sinCyc(float x) {
  x -= std::floor(x);
  if (x > 0.5f) x -= 1.0f;
  if (x > 0.25f) x = 0.5f - x;
  else if (x < -0.25f) x = -0.5f - x;
  const float t = x * 6.28318530718f, t2 = t * t;
  return t * (1.0f + t2 * (-0.16666667f + t2 * (0.008333333f + t2 * (-0.0001984127f + t2 * 2.7557319e-6f))));
}
inline float fastTanh(float x) {
  x = std::clamp(x, -3.0f, 3.0f);
  return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}
// Transparent below 0.6, then a smooth knee bounded by exactly 1.
inline float softClip(float x) {
  const float a = std::fabs(x);
  if (a <= 0.6f) return x;
  const float u = std::min((a - 0.6f) * 2.5f, 3.0f);
  const float y = 0.6f + 0.4f * (u * (27.0f + u * u) / (27.0f + 9.0f * u * u));
  const float c = std::min(y, 1.0f);
  return x < 0.0f ? -c : c;
}
inline float sat01(float x) { return std::clamp(x, 0.0f, 1.0f); }

// Smoothed macros and the values derived from them once per control step; shared by every slot.
struct Ctrl {
  float body = 0, strike = 0, bell = 0, warmth = 0, age = 0, crush = 0, res = 0, decay = 0, width = 0;
  float cutOct = 12.0f;
  float trimGain = 1.0f;
};

// Loudness compensation: measured (rms of a held default note while sweeping each macro alone), zero at the
// defaults. A macro should change colour, not level. Piecewise-linear dB through points at 0, .25, .5, .75, 1.
inline float trimCurveDb(const float (&pts)[5], float x) {
  const float p = std::clamp(x, 0.0f, 1.0f) * 4.0f;
  const int32_t k = std::min(static_cast<int32_t>(p), 3);
  return pts[k] + (pts[k + 1] - pts[k]) * (p - static_cast<float>(k));
}
inline float trimDb(const Ctrl& c) {
  static constexpr float kBody[5] = {2.8f, 1.2f, 0.1f, -1.05f, -2.0f};
  static constexpr float kStrike[5] = {-0.7f, -0.5f, 0.0f, 0.6f, 1.3f};
  static constexpr float kBell[5] = {0.4f, 0.2f, -0.1f, -0.3f, -0.3f};
  static constexpr float kWarmth[5] = {-0.73f, -0.31f, 0.08f, 0.23f, 0.22f};
  static constexpr float kCrush[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.1f};
  static constexpr float kRes[5] = {-0.8f, 0.5f, 1.4f, 2.0f, 2.4f};
  return trimCurveDb(kBody, c.body) + trimCurveDb(kStrike, c.strike) + trimCurveDb(kBell, c.bell) +
         trimCurveDb(kWarmth, c.warmth) + trimCurveDb(kCrush, c.crush) + trimCurveDb(kRes, c.res);
}

class Voice {
 public:
  bool active = false;
  bool releasing = false;

  void configure(double sampleRate) {
    sr_ = sampleRate;
    gate_.configure(sampleRate);
    gate_.reset();
    for (int32_t c = 0; c < 2; c++) filter_[c].configure(sampleRate);
    dcR_ = static_cast<float>(1.0 - kTwoPi * 6.0 / sampleRate);
    active = releasing = false;
  }

  void reset() {
    gate_.reset();
    active = releasing = false;
    delay_ = 0;
  }

  float gateLevel() const { return gate_.level(); }

  // `seed` makes the per-note variation (age) reproducible; `tail` = this slot was stolen mid-sound.
  void start(float velocity, int32_t delayFrames, float pitchSemis, uint32_t seed, const Ctrl& c,
             const AdsrParams& gate, bool stolen) {
    vel_ = std::isfinite(velocity) ? std::max(0.0f, velocity) : 0.0f;
    ratio_ = std::exp2(static_cast<double>(std::clamp(pitchSemis, -48.0f, 48.0f)) / 12.0);
    delay_ = stolen ? 0 : std::max(0, delayFrames);
    // The per-note character: a fixed tuning offset, a modulator phase, a bell tint and the phases of two slow
    // wanderers, all drawn from a hash of the note counter. Age scales how much of it is heard.
    auto h = [seed](uint32_t k) { return lfoSampleHold(static_cast<double>(seed * 8u + k)); };
    age_ = c.age;
    staticCents_ = h(0) * 4.0f;
    phi0_ = 0.06f * h(1);
    bellTint_ = h(2);
    wanderA_ = 0.5f + 0.5f * h(3);
    wanderB_ = 0.5f + 0.5f * h(4);
    wanderC_ = 0.5f + 0.5f * h(5);
    if (!stolen) {
      phA_ = 0.0;
      phB_ = 0.02 * age_ * h(6);
      phS_ = 0.0;
      phBm_ = 0.0;
      phBc_ = 0.0;
      phRing_ = 0.0;
      for (int32_t k = 0; k < 2; k++) {
        filter_[k].reset();
        dcX_[k] = dcY_[k] = 0.0f;
        bellLp_[k] = 0.0f;
        held_[k] = 0.0f;
      }
      holdAcc_ = 1.0f;
      noise_.reset();
      bp_.reset();
    }
    fast_ = slow_ = tine_ = bellEnv_ = ring_ = tick_ = 1.0f;
    cutInit_ = false;
    releasing = false;
    active = true;
    if (stolen) gate_.noteOnRestart(gate);
    else gate_.noteOn(gate);
  }

  void noteOff(const AdsrParams& gate) {
    if (!active || releasing) return;
    releasing = true;
    gate_.noteOff(gate);
  }

  // Renders n <= kSubBlock frames, accumulating into L/R. Clears `active` when the note has finished.
  void render(const Ctrl& c, double tuneHz, float* L, float* R, int32_t n) {
    int32_t j0 = 0;
    if (delay_ > 0) {
      if (delay_ >= n) {
        delay_ -= n;
        return;
      }
      j0 = delay_;
      delay_ = 0;
    }
    const int32_t m = n - j0;
    const float dt = static_cast<float>(m / sr_);
    const float invM = 1.0f / static_cast<float>(m);
    const float vel = std::min(vel_, 1.0f);
    const float velGain = vel_ * (0.7f + 0.3f * std::min(vel_, 1.0f));
    const float body = c.body, strike = c.strike, bell = c.bell, warmth = c.warmth, age = c.age;

    // --- time-based curves. Two-stage decay: a quick fall off the strike, then the long ring.
    const float tau = 0.12f * std::exp(6.03f * std::pow(c.decay, 1.5f));
    const float fastShare = 0.1f + 0.4f * strike;
    const float tauFast = std::max(0.03f, tau * 0.12f);
    const float tauBell = std::clamp(tau * 0.4f, 0.1f, 1.8f) * (0.75f + 0.5f * bell);
    const float tauTine = 0.035f * (1.0f + 1.5f * strike);
    const float tauRing = 0.05f;
    const float tauTick = 0.006f + 0.010f * strike;
    auto step = [&](float& s, float tauSec) {
      const float a = s;
      s *= std::exp(-dt / tauSec);
      return a;
    };
    const float decA0 = fastShare * fast_ + (1.0f - fastShare) * slow_;
    step(fast_, tauFast);
    step(slow_, tau);
    const float decA1 = fastShare * fast_ + (1.0f - fastShare) * slow_;
    const float tineA0 = tine_, bellA0 = bellEnv_, ringA0 = ring_, tickA0 = tick_;
    step(tine_, tauTine);
    step(bellEnv_, tauBell);
    step(ring_, tauRing);
    step(tick_, tauTick);
    const float tineA1 = tine_, bellA1 = bellEnv_, ringA1 = ring_, tickA1 = tick_;

    // --- per-note drift (age): pitch wander, tine-index wander, bell ratio wander.
    wanderA_ += static_cast<float>(0.23 * dt);
    wanderB_ += static_cast<float>(0.61 * dt);
    wanderC_ += static_cast<float>(0.37 * dt);
    wanderA_ -= std::floor(wanderA_);
    wanderB_ -= std::floor(wanderB_);
    wanderC_ -= std::floor(wanderC_);
    const float drift = sinCyc(wanderA_), driftB = sinCyc(wanderB_), driftC = sinCyc(wanderC_);
    const float cents = age * (staticCents_ + 5.0f * (0.65f * drift + 0.35f * driftB));
    const double f0 = std::clamp(tuneHz * ratio_ * (1.0 + cents * 0.0005778), 16.0, std::min(4200.0, sr_ * 0.2));
    const double inc = f0 / sr_;
    const float tineWander = 1.0f + 0.18f * age * driftC;

    // --- bell: inharmonic phase-modulation pair at 4 : 5.6, index bounded so the top sideband stays under ~0.42 sr.
    const double bellCar = 4.0, bellMod = 5.6 * (1.0 + 0.0035 * age * driftB);
    const float bellIdxMax = std::max(0.0f, static_cast<float>((0.42 * sr_ / f0 - 4.0) / 5.6 - 1.0));
    const float bellTint = 1.0f + 0.1f * age * bellTint_;
    const float bellIdxPeak =
        std::min(bellIdxMax, (0.2f + 3.0f * std::pow(bell, 1.3f)) * (0.35f + 0.65f * vel) * bellTint);
    const float bellAmpPeak = (0.04f + 0.5f * std::pow(bell, 1.1f)) * (0.6f + 0.4f * vel);
    const float bellLpA =
        1.0f - std::exp(static_cast<float>(-kTwoPi * 16000.0 * std::pow(0.28, warmth) / sr_));
    const bool ringOn = f0 * kRingRatio < 0.45 * sr_;
    const float ringPeak = ringOn ? 0.3f * bell * strike * (0.5f + 0.5f * vel) : 0.0f;

    // --- body weights.
    const float a1 = 0.30f + 0.70f * std::pow(body, 0.8f);
    const float a2 = 0.50f * (1.0f - 0.6f * body);
    const float subK = sat01((body - 0.5f) * 2.0f);
    const float a0 = 0.45f * subK * subK * (3.0f - 2.0f * subK);
    const float tineSus = 0.12f + 0.5f * (1.0f - body);
    const float tinePeak = (0.15f + 2.4f * std::pow(strike, 1.2f)) * (0.35f + 0.65f * vel);
    const float bMix = 0.25f + 0.35f * warmth;
    const double detB = 1.0 + (2.5 + 5.5 * warmth) * 0.0005778;
    const float ws = c.width;

    // --- tick (strike noise).
    bp_.set(std::clamp(f0 * 8.0, 2500.0, 8000.0), 0.8, sr_);
    const float tickPeak = 0.9f * std::pow(strike, 1.3f) * (0.2f + 0.8f * vel);

    // --- crush, warmth, filter.
    const float crush = c.crush;
    const float holdN = 1.0f + 8.0f * crush * crush;
    const float holdInc = 1.0f / holdN;
    const float bits = 15.0f - 9.0f * std::pow(crush, 1.1f);
    const float qL = std::exp2(bits - 1.0f), qInv = 1.0f / qL;
    const float qMod = std::exp2(8.0f - 5.5f * crush), qModInv = 1.0f / qMod;
    const bool crushOn = crush > 0.005f;
    const float g = 1.0f + 4.2f * std::pow(warmth, 1.2f);
    const float bias = 0.03f + 0.16f * warmth;
    const float biasT = fastTanh(bias);
    const float comp = kWarmRef / std::max(fastTanh(g * kWarmRef + bias) - biasT, 0.05f);

    // Cutoff: key-tracked, opened by the strike transient and a harder touch.
    const float keyOct = kKeyTrack * static_cast<float>(std::log2(f0 / 261.63));
    const float openOct = 1.1f * tineA1 * (0.3f + 0.7f * strike) * (0.4f + 0.6f * vel) + 1.0f * (vel - 0.7f);
    const float cutEnd = std::clamp(c.cutOct + keyOct + openOct, 6.3f, 14.4f);
    if (!cutInit_) {
      cutOct_ = cutEnd;
      cutInit_ = true;
    }
    const float cutHz = std::exp2(cutOct_);
    cutOct_ += 0.5f * (cutEnd - cutOct_);  // the strike opening falls fast: follow it within a few steps
    const float res = 0.04f + 0.86f * std::pow(c.res, 1.15f);

    const float outGain = velGain * kLevel * c.trimGain;

    for (int32_t j = j0; j < n; j++) {
      const float fr = static_cast<float>(j - j0 + 1) * invM;
      const float dec = decA0 + (decA1 - decA0) * fr;
      const float tineE = tineA0 + (tineA1 - tineA0) * fr;
      const float bellE = bellA0 + (bellA1 - bellA0) * fr;
      const float ringE = ringA0 + (ringA1 - ringA0) * fr;
      const float tickE = tickA0 + (tickA1 - tickA0) * fr;

      phA_ += inc;  phA_ -= std::floor(phA_);
      phB_ += inc * detB;  phB_ -= std::floor(phB_);
      phS_ += inc * 0.5;  phS_ -= std::floor(phS_);
      phBm_ += inc * bellMod;  phBm_ -= std::floor(phBm_);
      phBc_ += inc * bellCar;  phBc_ -= std::floor(phBc_);
      phRing_ += inc * kRingRatio;  phRing_ -= std::floor(phRing_);
      const float thA = static_cast<float>(phA_), thB = static_cast<float>(phB_);

      // Body: tine index = struck burst + a sustained part that follows the decay; the 2nd partial and the sub
      // fall at their own rates so the sound darkens as it rings.
      float mA = sinCyc(thA + phi0_ * age);
      float mB = sinCyc(thB + phi0_ * age * 1.7f + 0.05f);
      if (crushOn) {
        mA = std::floor(mA * qMod + 0.5f) * qModInv;
        mB = std::floor(mB * qMod + 0.5f) * qModInv;
      }
      const float It = std::min((tinePeak * tineE + tineSus * dec) * tineWander, 3.2f);
      const float d1 = dec, d2 = dec * dec, d0 = std::sqrt(dec);
      const float oscA = a1 * d1 * sinCyc(thA + It * kInvTwoPi * mA) + a2 * d2 * sinCyc(2.0f * thA);
      const float oscB = a1 * d1 * sinCyc(thB + 0.75f * It * kInvTwoPi * mB);
      const float sub = a0 > 0.0f ? a0 * d0 * sinCyc(static_cast<float>(phS_)) : 0.0f;
      const float mid = (1.0f - bMix) * oscA + bMix * oscB + sub;
      const float side = 0.35f * ws * (oscA - oscB);

      // Bell: two decorrelated modulators (right is offset in phase), one carrier phase.
      float bmL = sinCyc(static_cast<float>(phBm_));
      float bmR = sinCyc(static_cast<float>(phBm_) + 0.31f);
      if (crushOn) {
        bmL = std::floor(bmL * qMod + 0.5f) * qModInv;
        bmR = std::floor(bmR * qMod + 0.5f) * qModInv;
      }
      const float Ib = bellIdxPeak * bellE * kInvTwoPi;
      const float thC = static_cast<float>(phBc_);
      const float bcL = sinCyc(thC + Ib * bmL), bcR = sinCyc(thC + Ib * bmR);
      const float bMidV = 0.5f * (bcL + bcR);
      const float bAmp = bellAmpPeak * bellE;
      bellLp_[0] += bellLpA * (bMidV + ws * (bcL - bMidV) - bellLp_[0]);
      bellLp_[1] += bellLpA * (bMidV + ws * (bcR - bMidV) - bellLp_[1]);

      const float ringS = ringPeak > 0.0f ? ringPeak * ringE * sinCyc(static_cast<float>(phRing_)) : 0.0f;
      const float tickS = tickPeak * tickE * bp_.process(noise_.next());
      const float common = ringS + tickS;

      float x[2] = {mid + side + bAmp * bellLp_[0] + common, mid - side + bAmp * bellLp_[1] + common};

      if (crushOn) {
        holdAcc_ += holdInc;
        if (holdAcc_ >= 1.0f) {
          holdAcc_ -= 1.0f;
          for (int32_t k = 0; k < 2; k++) held_[k] = std::floor(std::clamp(x[k], -1.5f, 1.5f) * qL + 0.5f) * qInv;
        }
        x[0] = held_[0];
        x[1] = held_[1];
      }

      const float e = gate_.next();
      const float gain = e * e * outGain;
      for (int32_t k = 0; k < 2; k++) {
        float y = (fastTanh(g * x[k] + bias) - biasT) * comp;
        const float z = y - dcX_[k] + dcR_ * dcY_[k];
        dcX_[k] = y;
        dcY_[k] = z;
        y = filter_[k].process(z, cutHz, res, 0.0f, 0);
        (k == 0 ? L : R)[j] += y * gain;
      }
    }

    const bool gateDone = gate_.stage() == AdsrEnvelope::Stage::Idle;
    const float decNow = fastShare * fast_ + (1.0f - fastShare) * slow_;
    if (gateDone || vel_ <= 0.0f || (decNow < 1e-4f && tick_ < 1e-3f && tine_ < 1e-3f)) {
      active = false;
      releasing = false;
    }
  }

 private:
  static constexpr double kRingRatio = 7.09;
  static constexpr float kWarmRef = 0.35f;
  static constexpr float kKeyTrack = 0.35f;
  static constexpr float kLevel = 0.8f;

  double sr_ = 48000.0;
  AdsrEnvelope gate_;
  MatrixFilter filter_[2];
  PercNoise noise_;
  PercBandpass bp_;
  float vel_ = 0.0f;
  double ratio_ = 1.0;
  int32_t delay_ = 0;
  float age_ = 0, staticCents_ = 0, phi0_ = 0, bellTint_ = 0, wanderA_ = 0, wanderB_ = 0, wanderC_ = 0;
  double phA_ = 0, phB_ = 0, phS_ = 0, phBm_ = 0, phBc_ = 0, phRing_ = 0;
  float fast_ = 1, slow_ = 1, tine_ = 1, bellEnv_ = 1, ring_ = 1, tick_ = 1;
  float dcX_[2] = {}, dcY_[2] = {}, dcR_ = 0.999f, bellLp_[2] = {}, held_[2] = {}, holdAcc_ = 1.0f;
  float cutOct_ = 12.0f;
  bool cutInit_ = false;
};

}  // namespace electric

class ElectricInstrument final : public Instrument {
 public:
  ElectricInstrument() {
    for (int32_t i = 0; i < kElectricParamCount; i++) values_[i] = kElectricParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Electric; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    for (electric::Voice& v : slots_) v.configure(sampleRate);
    smoothSeconds_ = 0.012f;
    noteCounter_ = 0;
    snap_ = true;
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    const bool fresh = !core_.active;
    core_.begin(velocity, 0, pan, pitch);
    if (fresh) {
      core_.panner.snap(false);
      snap_ = true;
    }
    releaseHeld();
    electric::Voice* v = pickSlot();
    const bool stolen = v->active;
    v->start(velocity, delayFrames, pitch, ++noteCounter_, ctrl_, gateParams(), stolen);
  }

  void noteOff() override { releaseHeld(); }

  void setParam(int32_t param, float v) override {
    setTableParam(kElectricParams, kElectricParamCount, values_, param, v);
  }

  void reset() override {
    for (electric::Voice& v : slots_) v.reset();
    core_.clear();
    noteCounter_ = 0;
    snap_ = true;
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* L = scratch[0];
    float* R = scratch[1];
    std::fill(L, L + numFrames, 0.0f);
    std::fill(R, R + numFrames, 0.0f);
    const double sr = core_.sampleRate;
    const float dtBlock = static_cast<float>(electric::kSubBlock / sr);
    const float smoothA = 1.0f - std::exp(-dtBlock / smoothSeconds_);
    const double tuneHz = static_cast<double>(value(ElectricParam::Tune));

    for (int32_t i = 0; i < numFrames; i += electric::kSubBlock) {
      const int32_t n = std::min(electric::kSubBlock, numFrames - i);
      updateCtrl(smoothA);
      for (electric::Voice& v : slots_) {
        if (v.active) v.render(ctrl_, tuneHz, L + i, R + i, n);
      }
    }

    bool any = false;
    for (const electric::Voice& v : slots_) any = any || v.active;
    for (int32_t i = 0; i < numFrames; i++) {
      L[i] = electric::softClip(L[i]);
      R[i] = electric::softClip(R[i]);
    }
    const int32_t channels = std::min(busChannelCount, kMaxDspChannels);
    if (channels >= 2) {
      if (core_.panner.isUnity()) {
        for (int32_t i = 0; i < numFrames; i++) {
          busChannels[0][i] += L[i];
          busChannels[1][i] += R[i];
        }
      } else {
        core_.panner.process(L, R, busChannels[0], busChannels[1], numFrames, false, true);
      }
    } else if (channels == 1) {
      for (int32_t i = 0; i < numFrames; i++) busChannels[0][i] += 0.5f * (L[i] + R[i]);
    }
    if (!any) core_.active = false;
    return core_.active;
  }

 private:
  float value(ElectricParam p) const { return values_[static_cast<int32_t>(p)]; }

  AdsrParams gateParams() const {
    const float strike = value(ElectricParam::Strike);
    const float attack = 0.0015f + 0.04f * std::pow(1.0f - strike, 1.5f);
    const float release = value(ElectricParam::Release) * (0.5f + value(ElectricParam::Decay));
    return AdsrParams{attack, 0.0f, 1.0f, std::max(release, 0.015f)};
  }

  void releaseHeld() {
    const AdsrParams p = gateParams();
    for (electric::Voice& v : slots_) v.noteOff(p);
  }

  // A free slot; otherwise the quietest tail (a releasing one first).
  electric::Voice* pickSlot() {
    electric::Voice* best = nullptr;
    for (electric::Voice& v : slots_) {
      if (!v.active) return &v;
      if (!best || (v.releasing && !best->releasing) ||
          (v.releasing == best->releasing && v.gateLevel() < best->gateLevel())) {
        best = &v;
      }
    }
    return best;
  }

  void updateCtrl(float smoothA) {
    auto to = [&](float& s, float target) { s += smoothA * (target - s); };
    const float cutOct = std::log2(value(ElectricParam::Cutoff));
    if (snap_) {
      ctrl_.body = value(ElectricParam::Body);
      ctrl_.strike = value(ElectricParam::Strike);
      ctrl_.bell = value(ElectricParam::Bell);
      ctrl_.warmth = value(ElectricParam::Warmth);
      ctrl_.age = value(ElectricParam::Age);
      ctrl_.crush = value(ElectricParam::Crush);
      ctrl_.res = value(ElectricParam::Resonance);
      ctrl_.decay = value(ElectricParam::Decay);
      ctrl_.width = value(ElectricParam::Width);
      ctrl_.cutOct = cutOct;
      snap_ = false;
    } else {
      to(ctrl_.body, value(ElectricParam::Body));
      to(ctrl_.strike, value(ElectricParam::Strike));
      to(ctrl_.bell, value(ElectricParam::Bell));
      to(ctrl_.warmth, value(ElectricParam::Warmth));
      to(ctrl_.age, value(ElectricParam::Age));
      to(ctrl_.crush, value(ElectricParam::Crush));
      to(ctrl_.res, value(ElectricParam::Resonance));
      to(ctrl_.decay, value(ElectricParam::Decay));
      to(ctrl_.width, value(ElectricParam::Width));
      to(ctrl_.cutOct, cutOct);
    }
    ctrl_.trimGain = std::exp2(electric::trimDb(ctrl_) * 0.16609640474f);
  }

  float values_[kElectricParamCount] = {};
  PercCore core_;
  electric::Voice slots_[electric::kSlots];
  electric::Ctrl ctrl_;
  float smoothSeconds_ = 0.012f;
  uint32_t noteCounter_ = 0;
  bool snap_ = true;
};

}  // namespace webdsp
