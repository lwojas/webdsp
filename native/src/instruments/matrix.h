#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/fm_kernel.h"
#include "../dsp/matrix_filter.h"
#include "../dsp/percussion.h"

// Synthesized "Matrix": a subtractive voice (VCO -> VCF -> VCA) with a patch matrix. Behaviour,
// parameter mapping and client notes: docs/matrix-instrument.md. An original design.
//
//   VCO (saw | pulse | triangle) --+-- noise mix --> VCF (LP24 | LP12 | BP | HP) --> VCA --> soft clip --> bus
//
// The fixed path is the voice. The matrix is 7 sources x 6 destinations of signed amounts, each one
// an ordinary catalogue parameter (ids 22..63, matrixAmountParam()). Sources: LFO 1, LFO 2, ENV 1,
// ENV 2, noise, the VCO output and the VCF output. Destinations: VCO pitch, pulse width, filter
// cutoff, resonance, drive and VCA level. A destination's value is its own control plus the sum of
// (amount x source) over its patches, so a patch is "source, amount, destination" and nothing else.
//
// Audio-rate sources (noise, VCO, VCF) are patched exactly like LFOs and envelopes. A patch that
// carries a signal back to somewhere upstream of where it was taken (VCF out -> cutoff, VCF out ->
// pitch, VCO out -> pitch) is a feedback loop; it is closed by one sample of delay and bounded by
// the filter's limiter, so it cannot run away. Every other patch uses the source's current sample.
//
// Reused DSP: AdsrEnvelope (both envelopes), PercCore (lifecycle/pan/routing), PercNoise, the
// fmk sine table (LFOs), setTableParam. New: dsp/matrix_filter.h (per-sample retunable filter).
//
// Mono per instance; a retrigger is continuous (phases, filter state and envelope levels carry); a
// fresh note resets everything so identical inputs render identical samples. ENV 1 sets how long a
// note lives (a note ends when ENV 1 has released). Not tempo-synced. No allocation after configure().
namespace webdsp {

inline constexpr InstrumentParamInfo kMatrixParams[] = {
    {static_cast<int32_t>(MatrixParam::Tune), "tune", 27.5f, 1760.0f, 110.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(MatrixParam::Wave), "wave", 0.0f, 2.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(MatrixParam::Pwm), "pwm", 0.05f, 0.95f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Noise), "noise", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Cutoff), "cutoff", 20.0f, 18000.0f, 1200.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(MatrixParam::Resonance), "resonance", 0.0f, 1.0f, 0.25f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Drive), "drive", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Mode), "mode", 0.0f, 3.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(MatrixParam::Keytrack), "keytrack", 0.0f, 1.0f, 0.5f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Level), "level", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(MatrixParam::Lfo1Rate), "lfo1rate", 0.05f, 40.0f, 2.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(MatrixParam::Lfo1Wave), "lfo1wave", 0.0f, 4.0f, 0.0f, "step", ModDomain::None},
    {static_cast<int32_t>(MatrixParam::Lfo2Rate), "lfo2rate", 0.05f, 40.0f, 0.5f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(MatrixParam::Lfo2Wave), "lfo2wave", 0.0f, 4.0f, 1.0f, "step", ModDomain::None},
    {static_cast<int32_t>(MatrixParam::Env1Attack), "env1attack", 0.0005f, 4.0f, 0.003f, "s"},
    {static_cast<int32_t>(MatrixParam::Env1Decay), "env1decay", 0.01f, 4.0f, 0.4f, "s"},
    {static_cast<int32_t>(MatrixParam::Env1Sustain), "env1sustain", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(MatrixParam::Env1Release), "env1release", 0.005f, 4.0f, 0.12f, "s"},
    {static_cast<int32_t>(MatrixParam::Env2Attack), "env2attack", 0.0005f, 4.0f, 0.002f, "s"},
    {static_cast<int32_t>(MatrixParam::Env2Decay), "env2decay", 0.01f, 4.0f, 0.25f, "s"},
    {static_cast<int32_t>(MatrixParam::Env2Sustain), "env2sustain", 0.0f, 1.0f, 0.0f, ""},
    {static_cast<int32_t>(MatrixParam::Env2Release), "env2release", 0.005f, 4.0f, 0.12f, "s"},
    // The matrix: id = AmountBase + source * 6 + destination. -1..1, 0 = no patch.
    {matrixAmountParam(MatrixSource::Lfo1, MatrixDest::Pitch), "lfo1_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo1, MatrixDest::Pwm), "lfo1_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo1, MatrixDest::Cutoff), "lfo1_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo1, MatrixDest::Resonance), "lfo1_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo1, MatrixDest::Drive), "lfo1_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo1, MatrixDest::Level), "lfo1_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo2, MatrixDest::Pitch), "lfo2_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo2, MatrixDest::Pwm), "lfo2_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo2, MatrixDest::Cutoff), "lfo2_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo2, MatrixDest::Resonance), "lfo2_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo2, MatrixDest::Drive), "lfo2_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Lfo2, MatrixDest::Level), "lfo2_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env1, MatrixDest::Pitch), "env1_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env1, MatrixDest::Pwm), "env1_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env1, MatrixDest::Cutoff), "env1_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env1, MatrixDest::Resonance), "env1_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env1, MatrixDest::Drive), "env1_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env1, MatrixDest::Level), "env1_level", -1.0f, 1.0f, 1.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env2, MatrixDest::Pitch), "env2_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env2, MatrixDest::Pwm), "env2_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env2, MatrixDest::Cutoff), "env2_cutoff", -1.0f, 1.0f, 0.4f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env2, MatrixDest::Resonance), "env2_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env2, MatrixDest::Drive), "env2_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Env2, MatrixDest::Level), "env2_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Noise, MatrixDest::Pitch), "noise_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Noise, MatrixDest::Pwm), "noise_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Noise, MatrixDest::Cutoff), "noise_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Noise, MatrixDest::Resonance), "noise_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Noise, MatrixDest::Drive), "noise_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Noise, MatrixDest::Level), "noise_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vco, MatrixDest::Pitch), "vco_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vco, MatrixDest::Pwm), "vco_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vco, MatrixDest::Cutoff), "vco_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vco, MatrixDest::Resonance), "vco_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vco, MatrixDest::Drive), "vco_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vco, MatrixDest::Level), "vco_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vcf, MatrixDest::Pitch), "vcf_pitch", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vcf, MatrixDest::Pwm), "vcf_pwm", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vcf, MatrixDest::Cutoff), "vcf_cutoff", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vcf, MatrixDest::Resonance), "vcf_resonance", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vcf, MatrixDest::Drive), "vcf_drive", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
    {matrixAmountParam(MatrixSource::Vcf, MatrixDest::Level), "vcf_level", -1.0f, 1.0f, 0.0f, "", ModDomain::None},
};
inline constexpr int32_t kMatrixParamCount =
    static_cast<int32_t>(sizeof(kMatrixParams) / sizeof(kMatrixParams[0]));
static_assert(kMatrixParamCount == static_cast<int32_t>(MatrixParam::AmountBase) + kMatrixAmountCount,
              "the table is the 22 voice controls followed by every matrix cell, in id order");

class MatrixInstrument final : public Instrument {
 public:
  MatrixInstrument() {
    for (int32_t i = 0; i < kMatrixParamCount; i++) values_[i] = kMatrixParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Matrix; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    env1_.configure(sampleRate);
    env2_.configure(sampleRate);
    env1_.reset();
    env2_.reset();
    filter_.configure(sampleRate);
    sine_ = fmk::sineTable();
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    if (core_.begin(velocity, delayFrames, pan, pitch)) clearState();  // retrigger carries state
    env1_.noteOnRestart(envParams(0));
    env2_.noteOnRestart(envParams(1));
  }

  void duck(int32_t frames) override { env1_.duck(frames); }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    env1_.noteOff(envParams(0));
    env2_.noteOff(envParams(1));
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kMatrixParams, kMatrixParamCount, values_, param, v);
  }

  void reset() override {
    env1_.reset();
    env2_.reset();
    clearState();
    core_.clear();
  }

  bool isActive() const override { return core_.active; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!core_.active) return false;
    float* mono = scratch[0];
    const double sr = core_.sampleRate;

    const float velocity = std::min(core_.velocity, 1.0f);
    const float levelGain = velocity * kLevel;
    const double incRest = static_cast<double>(value(MatrixParam::Tune)) * core_.pitchRatio / sr;
    const int32_t wave = static_cast<int32_t>(value(MatrixParam::Wave) + 0.5f);
    const int32_t mode = static_cast<int32_t>(value(MatrixParam::Mode) + 0.5f);
    const int32_t lfo1Wave = static_cast<int32_t>(value(MatrixParam::Lfo1Wave) + 0.5f);
    const int32_t lfo2Wave = static_cast<int32_t>(value(MatrixParam::Lfo2Wave) + 0.5f);
    const double lfo1Inc = static_cast<double>(value(MatrixParam::Lfo1Rate)) / sr;
    const double lfo2Inc = static_cast<double>(value(MatrixParam::Lfo2Rate)) / sr;
    const float noiseMix = value(MatrixParam::Noise);
    const float keytrack = static_cast<float>(std::pow(core_.pitchRatio, static_cast<double>(value(MatrixParam::Keytrack))));

    // Pre-note delay: silence, and nothing advances.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t k = 0; k < lead; k++) mono[k] = 0.0f;
    core_.pendingDelay -= lead;

    bool finished = false;
    int32_t i = lead;
    while (i < numFrames) {
      const int32_t n = std::min(kSubBlock, numFrames - i);
      if (env1_.stage() == AdsrEnvelope::Stage::Idle ||
          (env1_.stage() == AdsrEnvelope::Stage::Sustain && env1_.level() <= 0.0f)) {
        finished = true;
        for (int32_t k = i; k < numFrames; k++) mono[k] = 0.0f;
        break;
      }

      // --- control rate: this step's patches (amounts glide so a change never clicks) and bases.
      gatherPatches();
      const float pwmBase = value(MatrixParam::Pwm);
      const float cutBase = value(MatrixParam::Cutoff) * keytrack;
      const float resBase = value(MatrixParam::Resonance);
      const float driveBase = value(MatrixParam::Drive);
      const float levelBase = value(MatrixParam::Level);
      const bool pitchMod = count_[kPitch] > 0, cutMod = count_[kCutoff] > 0;
      float cutNow = cutBase;

      // --- audio rate.
      for (int32_t k = 0; k < n; k++) {
        src_[static_cast<int32_t>(MatrixSource::Lfo1)] = lfo1_.next(lfo1Wave, lfo1Inc, sine_);
        src_[static_cast<int32_t>(MatrixSource::Lfo2)] = lfo2_.next(lfo2Wave, lfo2Inc, sine_);
        src_[static_cast<int32_t>(MatrixSource::Env1)] = env1_.next();
        src_[static_cast<int32_t>(MatrixSource::Env2)] = env2_.next();
        const float noise = noise_.next();
        src_[static_cast<int32_t>(MatrixSource::Noise)] = noise;
        // Upstream destinations hear the previous sample of the audio sources (the loop's one
        // sample of delay).
        src_[static_cast<int32_t>(MatrixSource::Vco)] = vcoPrev_;
        src_[static_cast<int32_t>(MatrixSource::Vcf)] = vcfPrev_;

        const float pw = std::clamp(pwmBase + kPwmDepth * sum(kPwm), 0.02f, 0.98f);
        double inc = incRest;
        if (pitchMod) inc *= std::exp2(static_cast<double>(std::clamp(sum(kPitch), -kMaxPitchOct, kMaxPitchOct)));
        inc = std::min(inc, kIncCap);
        const float vco = oscillator(wave, inc, pw);

        src_[static_cast<int32_t>(MatrixSource::Vco)] = vco;  // downstream of the VCO: current sample
        if (cutMod) {
          cutNow = cutBase * std::exp2(std::clamp(kCutoffOct * sum(kCutoff), -kMaxCutoffOct, kMaxCutoffOct));
        }
        const float res = count_[kRes] ? std::clamp(resBase + kResDepth * sum(kRes), 0.0f, 1.0f) : resBase;
        const float drv = count_[kDrive] ? std::clamp(driveBase + kDriveDepth * sum(kDrive), 0.0f, 1.0f) : driveBase;

        const float x = vco * (1.0f - 0.5f * noiseMix) + noise * (0.6f * noiseMix);
        const float vcf = filter_.process(x, cutNow, res, drv, mode);
        src_[static_cast<int32_t>(MatrixSource::Vcf)] = vcf;

        const float gain = std::clamp(levelBase + sum(kLevelDest), 0.0f, 1.0f);
        mono[i + k] = softClip(vcf * gain * gain * levelGain);
        vcoPrev_ = vco;
        vcfPrev_ = vcf;
      }
      i += n;
    }

    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr int32_t kPitch = static_cast<int32_t>(MatrixDest::Pitch);
  static constexpr int32_t kPwm = static_cast<int32_t>(MatrixDest::Pwm);
  static constexpr int32_t kCutoff = static_cast<int32_t>(MatrixDest::Cutoff);
  static constexpr int32_t kRes = static_cast<int32_t>(MatrixDest::Resonance);
  static constexpr int32_t kDrive = static_cast<int32_t>(MatrixDest::Drive);
  static constexpr int32_t kLevelDest = static_cast<int32_t>(MatrixDest::Level);

  static constexpr float kLevel = 0.9f;
  static constexpr int32_t kSubBlock = 32;        // control-rate step (~0.67 ms at 48 kHz)
  static constexpr float kAmountGlide = 0.35f;    // per step; ~2 ms to settle
  static constexpr double kIncCap = 0.45;         // instantaneous frequency < 0.45 fs
  // What a 100% patch does (a patch of amount a moves the destination by a x source x depth).
  static constexpr float kMaxPitchOct = 6.0f;     // pitch: 100% = 1 octave
  static constexpr float kPwmDepth = 0.45f;       // pulse width: 100% = +-0.45
  static constexpr float kCutoffOct = 5.0f;       // cutoff: 100% = 5 octaves
  static constexpr float kMaxCutoffOct = 9.0f;
  static constexpr float kResDepth = 0.5f;        // resonance: 100% = +-0.5
  static constexpr float kDriveDepth = 0.6f;      // drive: 100% = +-0.6

  static int32_t idx(MatrixParam p) { return static_cast<int32_t>(p); }
  float value(MatrixParam p) const { return values_[idx(p)]; }

  AdsrParams envParams(int32_t env) const {
    const MatrixParam a = env == 0 ? MatrixParam::Env1Attack : MatrixParam::Env2Attack;
    const int32_t b = idx(a);
    return AdsrParams{values_[b], values_[b + 1], values_[b + 2], values_[b + 3]};
  }

  // Steps the glided amounts toward the parameter values and rebuilds the per-destination lists of
  // live patches (so a silent matrix costs nothing per sample).
  void gatherPatches() {
    const int32_t base = idx(MatrixParam::AmountBase);
    for (int32_t d = 0; d < kMatrixDestCount; d++) {
      int32_t n = 0;
      for (int32_t s = 0; s < kMatrixSourceCount; s++) {
        const float target = values_[base + s * kMatrixDestCount + d];
        float& cur = amount_[s][d];
        cur = snapAmounts_ ? target : cur + (target - cur) * kAmountGlide;
        if (target == 0.0f && std::fabs(cur) < 1e-4f) cur = 0.0f;
        if (cur != 0.0f) {
          patchSrc_[d][n] = s;
          patchAmt_[d][n] = cur;
          n++;
        }
      }
      count_[d] = n;
    }
    snapAmounts_ = false;
  }

  float sum(int32_t d) const {
    float t = 0.0f;
    for (int32_t j = 0; j < count_[d]; j++) t += patchAmt_[d][j] * src_[patchSrc_[d][j]];
    return t;
  }

  // Band-limited (PolyBLEP) saw and pulse; the triangle is naive (its harmonics fall as 1/n^2).
  float oscillator(int32_t wave, double inc, float width) {
    phase_ += inc;
    phase_ -= std::floor(phase_);
    const double p = phase_;
    if (wave == 1) {
      double q = p - static_cast<double>(width);
      q -= std::floor(q);
      const float s = (p < width ? 1.0f : -1.0f) + polyBlep(p, inc) - polyBlep(q, inc);
      return s - (2.0f * width - 1.0f);  // remove the DC a narrow pulse carries
    }
    if (wave == 2) return static_cast<float>(1.0 - 4.0 * std::fabs(p - 0.5));
    return static_cast<float>(2.0 * p - 1.0) - polyBlep(p, inc);
  }

  static float polyBlep(double t, double dt) {
    if (t < dt) {
      const double x = t / dt;
      return static_cast<float>(x + x - x * x - 1.0);
    }
    if (t > 1.0 - dt) {
      const double x = (t - 1.0) / dt;
      return static_cast<float>(x * x + x + x + 1.0);
    }
    return 0.0f;
  }

  void clearState() {
    phase_ = 0.0;
    vcoPrev_ = vcfPrev_ = 0.0f;
    lfo1_.reset(0x2545F491u);
    lfo2_.reset(0x9E3779B1u);
    noise_.reset();
    filter_.reset();
    for (auto& row : amount_) for (float& a : row) a = 0.0f;
    snapAmounts_ = true;
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

  // Free-running bipolar LFO; sample & hold takes a new value each cycle.
  struct Lfo {
    double phase = 0.0;
    float held = 0.0f;
    uint32_t rng = 1;
    void reset(uint32_t seed) {
      phase = 0.0;
      rng = seed;
      held = draw();
    }
    float draw() {
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      return static_cast<float>(static_cast<int32_t>(rng)) * (1.0f / 2147483648.0f);
    }
    float next(int32_t wave, double inc, const float* sine) {
      const double p = phase;
      phase += inc;
      if (phase >= 1.0) {
        phase -= std::floor(phase);
        held = draw();
      }
      switch (wave) {
        case 0: return fmk::sinTurns(sine, p);
        case 1: return static_cast<float>(p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0));
        case 2: return static_cast<float>(2.0 * p - 1.0);
        case 3: return p < 0.5 ? 1.0f : -1.0f;
        default: return held;
      }
    }
  };

  float values_[kMatrixParamCount] = {};
  PercCore core_;
  AdsrEnvelope env1_, env2_;
  MatrixFilter filter_;
  PercNoise noise_;
  Lfo lfo1_, lfo2_;
  const float* sine_ = nullptr;
  double phase_ = 0.0;
  float vcoPrev_ = 0.0f, vcfPrev_ = 0.0f;
  float src_[kMatrixSourceCount] = {};
  float amount_[kMatrixSourceCount][kMatrixDestCount] = {};
  float patchAmt_[kMatrixDestCount][kMatrixSourceCount] = {};
  int32_t patchSrc_[kMatrixDestCount][kMatrixSourceCount] = {};
  int32_t count_[kMatrixDestCount] = {};
  bool snapAmounts_ = true;
};

}  // namespace webdsp
