#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adaa_tanh.h"
#include "../dsp/adsr_envelope.h"
#include "../dsp/character_filter.h"  // Oversampler2x
#include "../dsp/harmonic_osc.h"
#include "../dsp/matrix_filter.h"
#include "../dsp/percussion.h"

// SubBass: a harmonic sine / sub-bass voice. Shipped behaviour and parameter mapping: docs/subbass-instrument.md.
//
//   HarmonicOsc (sine -> saw-like, band-limited additive)  <- pitch = Root * octave * tune * note * pitch env
//        |  Shape / Profile
//   2x oversampled AdaaTanh drive (linear and clean at 0; the half-band delay is constant, ~0.3 ms)
//        |
//   MatrixFilter LP 24 / 12 dB <- cutoff = Cutoff + filter env (octaves), resonance
//        |
//   x Level x amp env^2 x velocity -> soft clip (bounded +-1) -> Panner -> bus
//
// Shape is the oscillator's own harmonic character; Drive is nonlinear saturation after it, so the two are not
// the same control: Shape 0 and Drive 0 is a sine with no harmonics at all, and Drive on a sine adds odd
// harmonics that depend on level, while Shape adds a fixed, chosen spectrum.
//
// Reused: HarmonicOsc / AdaaTanh (new reusable primitives in dsp/), Oversampler2x (from the Character Filter), MatrixFilter (ZDF ladder + SVF, stable under
// per-sample retuning), AdsrEnvelope (amp and filter), PercCore (lifecycle, pan, pre-note delay).
//
// Mono per instance. A fresh note resets the filter and pitch envelope and, with PhaseReset, the phase, so
// identical inputs render identical samples. A retrigger of a sounding note is continuous: phase and filter
// carry and the amp envelope fades out over ~2 ms before the new attack.
namespace webdsp {

inline constexpr InstrumentParamInfo kSubBassParams[] = {
    {static_cast<int32_t>(SubBassParam::Shape), "shape", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(SubBassParam::Profile), "profile", 0.0f, 4.0f, 0.0f, "step"},
    {static_cast<int32_t>(SubBassParam::Octave), "octave", -2.0f, 1.0f, 0.0f, "step"},
    {static_cast<int32_t>(SubBassParam::Tune), "tune", -100.0f, 100.0f, 0.0f, "ct", ModDomain::Linear},
    {static_cast<int32_t>(SubBassParam::Level), "level", 0.0f, 1.0f, 0.85f, "", ModDomain::Linear},
    {static_cast<int32_t>(SubBassParam::Phase), "phase", 0.0f, 1.0f, 0.0f, "", ModDomain::None},
    {static_cast<int32_t>(SubBassParam::PhaseReset), "phasereset", 0.0f, 1.0f, 1.0f, "", ModDomain::None},
    {static_cast<int32_t>(SubBassParam::Drive), "drive", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(SubBassParam::FilterType), "filtertype", 0.0f, 1.0f, 0.0f, "step"},
    {static_cast<int32_t>(SubBassParam::Cutoff), "cutoff", 30.0f, 16000.0f, 12000.0f, "Hz", ModDomain::Log},
    {static_cast<int32_t>(SubBassParam::Resonance), "resonance", 0.0f, 1.0f, 0.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(SubBassParam::FilterAttack), "fattack", 0.001f, 2.0f, 0.002f, "s"},
    {static_cast<int32_t>(SubBassParam::FilterDecay), "fdecay", 0.01f, 4.0f, 0.3f, "s"},
    {static_cast<int32_t>(SubBassParam::FilterSustain), "fsustain", 0.0f, 1.0f, 0.3f, "", ModDomain::Linear},
    {static_cast<int32_t>(SubBassParam::FilterRelease), "frelease", 0.01f, 4.0f, 0.2f, "s"},
    {static_cast<int32_t>(SubBassParam::FilterAmount), "famount", -5.0f, 5.0f, 0.0f, "oct", ModDomain::Linear},
    {static_cast<int32_t>(SubBassParam::PitchAmount), "pitchamount", -24.0f, 24.0f, 0.0f, "st", ModDomain::Linear},
    {static_cast<int32_t>(SubBassParam::PitchDecay), "pitchdecay", 0.005f, 0.5f, 0.08f, "s"},
    {static_cast<int32_t>(SubBassParam::AmpAttack), "attack", 0.001f, 2.0f, 0.003f, "s"},
    {static_cast<int32_t>(SubBassParam::AmpDecay), "decay", 0.01f, 4.0f, 0.3f, "s"},
    {static_cast<int32_t>(SubBassParam::AmpSustain), "sustain", 0.0f, 1.0f, 1.0f, "", ModDomain::Linear},
    {static_cast<int32_t>(SubBassParam::AmpRelease), "release", 0.01f, 4.0f, 0.25f, "s"},
    {static_cast<int32_t>(SubBassParam::Root), "root", 20.0f, 880.0f, 55.0f, "Hz", ModDomain::Log},
};
inline constexpr int32_t kSubBassParamCount =
    static_cast<int32_t>(sizeof(kSubBassParams) / sizeof(kSubBassParams[0]));

class SubBassInstrument final : public Instrument {
 public:
  SubBassInstrument() {
    for (int32_t i = 0; i < kSubBassParamCount; i++) values_[i] = kSubBassParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::SubBass; }

  void configure(double sampleRate) override {
    core_.configure(sampleRate);
    ampEnv_.configure(sampleRate);
    ampEnv_.reset();
    filtEnv_.configure(sampleRate);
    filtEnv_.reset();
    filter_.configure(sampleRate);
    clearState();
  }

  void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) override {
    const bool fresh = core_.begin(velocity, delayFrames, pan, pitch);
    if (fresh) clearState();  // also puts the phase at Phase when PhaseReset is on
    pitchEnv_ = 1.0f;  // the pitch transient restarts on every note
    ampEnv_.noteOnRestart(ampParams());
    filtEnv_.noteOn(filterParams());
  }

  void duck(int32_t frames) override { ampEnv_.duck(frames); }

  void noteOff() override {
    if (!core_.active || core_.releasing) return;
    core_.releasing = true;
    ampEnv_.noteOff(ampParams());
    filtEnv_.noteOff(filterParams());
  }

  void setParam(int32_t param, float v) override {
    setTableParam(kSubBassParams, kSubBassParamCount, values_, param, v);
  }

  void reset() override {
    ampEnv_.reset();
    filtEnv_.reset();
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

    const int32_t profile = std::clamp(static_cast<int32_t>(std::lround(value(SubBassParam::Profile))), 0, kHarmonicProfileCount - 1);
    const int32_t octave = std::clamp(static_cast<int32_t>(std::lround(value(SubBassParam::Octave))), -2, 1);
    const int32_t mode = value(SubBassParam::FilterType) >= 0.5f ? 1 : 0;
    if (mode != filterMode_) {
      // Ladder and state-variable filter keep different state: crossfade from the old one for a few ms.
      oldFilter_ = filter_;
      oldMode_ = filterMode_;
      filter_.reset();
      filterMode_ = mode;
      modeFade_ = kModeFadeFrames;
    }
    const double baseSemis = 12.0 * octave + value(SubBassParam::Tune) * 0.01;
    const double rootHz = static_cast<double>(value(SubBassParam::Root));
    const float dtBlock = static_cast<float>(kSubBlock / sr);
    const float smoothA = 1.0f - std::exp(-dtBlock / kSmoothSeconds);
    const float pitchFall = std::exp(-dtBlock / (std::max(value(SubBassParam::PitchDecay), 0.001f) * kDecayToTau));
    const float pitchAmt = value(SubBassParam::PitchAmount);
    const float famount = value(SubBassParam::FilterAmount);
    const float vel = std::min(core_.velocity, 1.0f);
    const float velTarget = vel * (0.7f + 0.3f * vel);  // soft notes are quieter and silent at 0
    const float velA = 1.0f - std::exp(-dtBlock / kVelocitySeconds);

    // Pre-note delay: silence, and nothing advances.
    const int32_t lead = std::min(core_.pendingDelay, numFrames);
    for (int32_t j = 0; j < lead; j++) mono[j] = gains[j] = 0.0f;
    core_.pendingDelay -= lead;

    bool finished = false;
    int32_t i = lead;
    while (i < numFrames) {
      const int32_t n = std::min(kSubBlock, numFrames - i);
      if (ampEnv_.stage() == AdsrEnvelope::Stage::Idle ||
          (ampEnv_.stage() == AdsrEnvelope::Stage::Sustain && ampEnv_.level() <= 0.0f)) {
        finished = true;
        for (int32_t j = i; j < numFrames; j++) mono[j] = gains[j] = 0.0f;
        break;
      }

      // --- control rate. Targets are smoothed so macro changes never zipper.
      if (snap_) {
        sShape_ = value(SubBassParam::Shape);
        sDrive_ = value(SubBassParam::Drive);
        sLevel_ = value(SubBassParam::Level);
        sCutOct_ = std::log2(value(SubBassParam::Cutoff));
        sRes_ = value(SubBassParam::Resonance);
        sVel_ = velTarget;
        gainNow_ = -1.0f;
        snap_ = false;
      } else {
        sShape_ += smoothA * (value(SubBassParam::Shape) - sShape_);
        sDrive_ += smoothA * (value(SubBassParam::Drive) - sDrive_);
        sLevel_ += smoothA * (value(SubBassParam::Level) - sLevel_);
        sCutOct_ += smoothA * (std::log2(value(SubBassParam::Cutoff)) - sCutOct_);
        sRes_ += smoothA * (value(SubBassParam::Resonance) - sRes_);
        sVel_ += velA * (velTarget - sVel_);  // a retrigger's new velocity must not step the level
      }

      pitchEnv_ *= pitchFall;
      if (pitchEnv_ < 1e-5f) pitchEnv_ = 0.0f;
      const double semis = baseSemis + 12.0 * std::log2(core_.pitchRatio) + static_cast<double>(pitchAmt) * pitchEnv_;
      const double f0 = std::clamp(rootHz * std::exp2(semis / 12.0), 10.0, sr * 0.25);
      const double incEnd = f0 / sr;
      if (!incInit_) { inc_ = incEnd; incInit_ = true; }
      const double incStart = inc_;
      const double incStep = (incEnd - incStart) / n;
      // Drive adds harmonics of its own (and is oversampled 2x), so the oscillator leaves a little more room below Nyquist as it goes up.
      const float drive = std::clamp(sDrive_, 0.0f, 1.0f);
      osc_.setSpectrum(profile, sShape_, std::max(incStart, incEnd), n, kHarmonicBandLimit - 0.12f * drive);

      // Drive 0..1 -> tanh gain 0..12, curved so the low end of the knob is gentle. Peak stays at full scale
      // for any gain, so the extra loudness is only the added body; trim it a little.
      sat_.setGain(drive <= 0.0f ? 0.0f : 12.0f * std::pow(drive, 1.6f));
      const float driveTrim = 1.0f - 0.22f * drive;

      const float res = std::clamp(0.92f * sRes_, 0.0f, 0.92f);
      const float levelGain = sVel_ * kOutLevel * sLevel_ * driveTrim * kFilterInvScale;
      const float cutLo = std::log2(30.0f);
      const float cutHi = static_cast<float>(std::log2(sr * 0.44));

      if (gainNow_ < 0.0f) gainNow_ = levelGain;
      const float gainStep = (levelGain - gainNow_) / static_cast<float>(n);

      for (int32_t j = 0; j < n; j++) {
        float y = osc_.next(incStart + incStep * (j + 1));
        float oa, ob;
        os_.up(y, oa, ob);
        y = os_.down(sat_.process(oa), sat_.process(ob));
        const float fe = filtEnv_.next();
        const float cutOct = std::clamp(sCutOct_ + famount * fe, cutLo, cutHi);
        const float fin = y * kFilterInScale, fc = std::exp2(cutOct);
        y = filter_.process(fin, fc, res, 0.0f, filterMode_);
        if (modeFade_ > 0) {
          // The other filter type was swapped out: its state would step the signal, so it fades away instead.
          const float w = static_cast<float>(modeFade_) / kModeFadeFrames;
          y += w * (oldFilter_.process(fin, fc, res, 0.0f, oldMode_) - y);
          modeFade_--;
        }
        const float e = ampEnv_.next();
        mono[i + j] = y;
        gainNow_ += gainStep;
        gains[i + j] = e * e * gainNow_;
      }
      inc_ = incEnd;
      i += n;
    }

    for (int32_t j = lead; j < numFrames; j++) mono[j] = softClip(mono[j] * gains[j]);
    core_.route(mono, busChannels, busChannelCount, numFrames);
    if (finished) core_.active = false;
    return core_.active;
  }

 private:
  static constexpr int32_t kSubBlock = 16;       // control-rate step (~0.33 ms at 48 kHz)
  static constexpr float kSmoothSeconds = 0.012f;
  static constexpr float kVelocitySeconds = 0.002f;
  static constexpr int32_t kModeFadeFrames = 480;  // 10 ms at 48 kHz
  static constexpr float kOutLevel = 0.6f;       // a Level 1 sine peaks at 0.6: the bottom of the soft clip's knee
  // The filter's saturators are rational tanh: feeding them a small signal and scaling back keeps a sine
  // through them clean (they only bite on resonant peaks).
  static constexpr float kFilterInScale = 0.04f;
  static constexpr float kFilterInvScale = 1.0f / kFilterInScale;

  static int32_t idx(SubBassParam p) { return static_cast<int32_t>(p); }
  float value(SubBassParam p) const { return values_[idx(p)]; }

  AdsrParams ampParams() const {
    return AdsrParams{value(SubBassParam::AmpAttack), value(SubBassParam::AmpDecay), value(SubBassParam::AmpSustain),
                      value(SubBassParam::AmpRelease)};
  }
  AdsrParams filterParams() const {
    return AdsrParams{value(SubBassParam::FilterAttack), value(SubBassParam::FilterDecay),
                      value(SubBassParam::FilterSustain), value(SubBassParam::FilterRelease)};
  }

  void clearState() {
    sat_.reset();
    os_.reset();
    filter_.reset();
    filterMode_ = value(SubBassParam::FilterType) >= 0.5f ? 1 : 0;
    modeFade_ = 0;
    filtEnv_.reset();
    pitchEnv_ = 1.0f;
    incInit_ = false;
    snap_ = true;
    if (value(SubBassParam::PhaseReset) >= 0.5f) osc_.reset(value(SubBassParam::Phase));
  }

  // Transparent below 0.6, then a smooth knee bounded by exactly 1.
  static float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.6f) return x;
    const float u = std::min((a - 0.6f) * 2.5f, 3.0f);
    const float y = 0.6f + 0.4f * (u * (27.0f + u * u) / (27.0f + 9.0f * u * u));
    const float c = std::min(y, 1.0f);
    return x < 0.0f ? -c : c;
  }

  float values_[kSubBassParamCount] = {};
  PercCore core_;
  AdsrEnvelope ampEnv_, filtEnv_;
  MatrixFilter filter_, oldFilter_;
  int32_t filterMode_ = 0, oldMode_ = 0, modeFade_ = 0;
  HarmonicOsc osc_;
  AdaaTanh sat_;
  Oversampler2x os_;

  float sShape_ = 0.0f, sDrive_ = 0.0f, sLevel_ = 0.0f, sCutOct_ = 13.0f, sRes_ = 0.0f, sVel_ = 0.0f, gainNow_ = -1.0f;
  float pitchEnv_ = 1.0f;
  double inc_ = 0.0;
  bool incInit_ = false, snap_ = true;
};

}  // namespace webdsp
