#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../dsp/adsr_envelope.h"
#include "../dsp/biquad_filter.h"
#include "../dsp/panner.h"
#include "../dsp/saturation.h"
#include "../instrument.h"

// Synthesized kick drum: a virtual-analog-style "bridged-T" kick reduced to its essentials.
// Full write-up: docs/kick-instrument.md.
//
//   sine osc  <- pitch env (exp. decay, linear in Hz: f = tune * (1 + (2^drop - 1) * e))
//      |
//      +  click (deterministic noise burst, ~2 ms decay, low-passed)   <- Punch
//      |
//   amp env (AdsrEnvelope, applied squared for a more exponential tail)  <- Decay
//      |
//   Saturation (tanh + slight asymmetry, level-compensated)             <- Drive
//      |
//   Panner -> bus
//
// Reused engine DSP: AdsrEnvelope (amp), BiquadFilter (click shaping), Saturation (drive),
// Panner (placement). The only new DSP is the sine oscillator and the one-pole pitch/click
// decays, which the engine's linear-segment ADSR can't express.
//
// Everything is deterministic: phase and noise seed are reset on a fresh note, so identical
// inputs render identical samples.
namespace webdsp {

inline constexpr InstrumentParamInfo kKickParams[] = {
    {static_cast<int32_t>(KickParam::Tune), "tune", 30.0f, 120.0f, 50.0f, "Hz"},
    {static_cast<int32_t>(KickParam::PitchDrop), "pitchDrop", 0.0f, 4.0f, 2.5f, "oct"},
    {static_cast<int32_t>(KickParam::PitchDecay), "pitchDecay", 0.005f, 0.25f, 0.04f, "s"},
    {static_cast<int32_t>(KickParam::Decay), "decay", 0.05f, 2.0f, 0.45f, "s"},
    {static_cast<int32_t>(KickParam::Punch), "punch", 0.0f, 1.0f, 0.5f, ""},
    {static_cast<int32_t>(KickParam::Drive), "drive", 0.0f, 1.0f, 0.3f, ""},
};
inline constexpr int32_t kKickParamCount =
    static_cast<int32_t>(sizeof(kKickParams) / sizeof(kKickParams[0]));

class KickInstrument final : public Instrument {
 public:
  KickInstrument() {
    for (int32_t i = 0; i < kKickParamCount; i++) values_[i] = kKickParams[i].def;
  }

  InstrumentKind kind() const override { return InstrumentKind::Kick; }

  void configure(double sampleRate) override {
    sampleRate_ = sampleRate;
    env_.configure(sampleRate);
    env_.reset();
    panner_.configure(sampleRate);
    clickFilter_ = BiquadFilter(sampleRate);
    clickFilter_.setParam(static_cast<int32_t>(VoiceParam::FilterCutoff), kClickCutoffHz);
    saturation_ = Saturation(sampleRate);
    saturation_.reset();
    applyDrive();
    active_ = false;
  }

  void noteOn(float velocity, int32_t delayFrames, float pan) override {
    velocity_ = std::isfinite(velocity) ? std::max(0.0f, velocity) : 0.0f;
    pendingDelay_ = std::max(0, delayFrames);
    panner_.setPan(std::isfinite(pan) ? pan : 0.0f);
    if (!active_) {
      // Fresh note: start at the sine's peak so the (0.5 ms) attack ramp, not the phase, is
      // what avoids a click. A retrigger keeps phase/envelope level and just restarts the
      // pitch sweep — continuous waveform, no discontinuity.
      phase_ = 0.25;
      panner_.snap(true);
      saturation_.reset();
      clickFilter_.reset();
    }
    pitchEnv_ = 1.0f;
    clickEnv_ = 1.0f;
    noise_ = kNoiseSeed;
    env_.noteOn(AdsrParams{kAttackSeconds, value(KickParam::Decay), 0.0f, kReleaseSeconds});
    active_ = true;
    releasing_ = false;
  }

  void noteOff() override {
    if (!active_ || releasing_) return;
    releasing_ = true;
    env_.noteOff(AdsrParams{kAttackSeconds, value(KickParam::Decay), 0.0f, kReleaseSeconds});
  }

  void setParam(int32_t param, float v) override {
    if (!std::isfinite(v)) return;
    for (int32_t i = 0; i < kKickParamCount; i++) {
      if (kKickParams[i].id != param) continue;
      values_[i] = std::clamp(v, kKickParams[i].min, kKickParams[i].max);
      if (param == static_cast<int32_t>(KickParam::Drive)) applyDrive();
      return;
    }
  }

  void reset() override {
    env_.reset();
    saturation_.reset();
    clickFilter_.reset();
    active_ = false;
    releasing_ = false;
    pendingDelay_ = 0;
  }

  bool isActive() const override { return active_; }

  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) override {
    if (!active_) return false;
    float* body = scratch[0];
    float* click = scratch[1];

    const double tune = value(KickParam::Tune);
    const double extra = std::exp2(static_cast<double>(value(KickParam::PitchDrop))) - 1.0;
    const double nyquistGuard = sampleRate_ * 0.45;
    const float pitchCoef = decayCoef(value(KickParam::PitchDecay));
    const float clickCoef = decayCoef(kClickSeconds);
    const float punch = value(KickParam::Punch);
    const float gain = velocity_ * kBodyLevel;
    const double twoPi = 6.283185307179586;

    // The saturator (asymmetric) emits a DC transient even from silence, so the pre-note
    // delay is kept out of it: only frames from `live` onward go through click filter/drive.
    const int32_t live = std::min(pendingDelay_, numFrames);
    bool finished = false;
    for (int32_t i = 0; i < numFrames; i++) {
      if (pendingDelay_ > 0) {
        pendingDelay_--;
        body[i] = click[i] = 0.0f;
        continue;
      }
      if (!finished &&
          (!env_.isActive() || (env_.stage() == AdsrEnvelope::Stage::Sustain && env_.level() <= 0.0f))) {
        finished = true;
      }
      if (finished) {
        body[i] = click[i] = 0.0f;
        continue;
      }
      const float e = env_.next();
      const double freq = std::min(tune * (1.0 + extra * pitchEnv_), nyquistGuard);
      pitchEnv_ *= pitchCoef;
      phase_ += freq / sampleRate_;
      phase_ -= std::floor(phase_);
      body[i] = static_cast<float>(std::sin(twoPi * phase_)) * e * e * gain;

      noise_ ^= noise_ << 13;
      noise_ ^= noise_ >> 17;
      noise_ ^= noise_ << 5;
      const float n = static_cast<float>(static_cast<int32_t>(noise_)) * (1.0f / 2147483648.0f);
      click[i] = n * clickEnv_ * punch * kClickLevel * velocity_;
      clickEnv_ *= clickCoef;
    }

    // Click is shaped (low-passed) as its own layer, so the transient stays a "thump-tick"
    // rather than hiss, then summed with the body ahead of the saturator.
    const int32_t liveFrames = numFrames - live;
    float* clickCh[1] = {click + live};
    clickFilter_.process(clickCh, 1, liveFrames);
    for (int32_t i = live; i < numFrames; i++) body[i] += click[i];

    float* monoCh[1] = {body + live};
    saturation_.process(monoCh, 1, liveFrames);

    const int32_t channels = std::min(busChannelCount, kMaxDspChannels);
    if (channels >= 2 && !panner_.isUnity()) {
      panner_.process(body, body, busChannels[0], busChannels[1], numFrames,
                      /*monoSource=*/true, /*accumulate=*/true);
    } else {
      for (int32_t c = 0; c < channels; c++) {
        for (int32_t i = 0; i < numFrames; i++) busChannels[c][i] += body[i];
      }
    }

    if (finished) active_ = false;
    return active_;
  }

 private:
  static constexpr float kAttackSeconds = 0.0005f;
  static constexpr float kReleaseSeconds = 0.008f;
  static constexpr float kClickSeconds = 0.002f;
  static constexpr float kClickCutoffHz = 3500.0f;
  static constexpr float kBodyLevel = 0.8f;
  static constexpr float kClickLevel = 0.3f;
  static constexpr uint32_t kNoiseSeed = 0x9E3779B9u;

  float value(KickParam p) const { return values_[static_cast<int32_t>(p)]; }

  // Per-sample multiplier giving a one-pole decay with time constant `seconds`.
  float decayCoef(float seconds) const {
    return static_cast<float>(std::exp(-1.0 / (static_cast<double>(seconds) * sampleRate_)));
  }

  // Drive 0..1 -> 0..24 dB of pre-gain with mild even-harmonic asymmetry, and a level trim so
  // the saturated kick doesn't get dramatically louder than the clean one. Drive 0 bypasses.
  void applyDrive() {
    const float d = value(KickParam::Drive);
    const float driveDb = d * 24.0f;
    saturation_.setParam(static_cast<int32_t>(NodeParam::SatDrive), driveDb);
    saturation_.setParam(static_cast<int32_t>(NodeParam::SatAsymmetry), 0.25f * d);
    saturation_.setParam(static_cast<int32_t>(NodeParam::SatOutputGain), -0.35f * driveDb);
    saturation_.setParam(static_cast<int32_t>(NodeParam::SatMix), d > 0.0f ? 1.0f : 0.0f);
  }

  double sampleRate_ = 48000.0;
  float values_[kKickParamCount] = {};
  float velocity_ = 1.0f;
  bool active_ = false;
  bool releasing_ = false;
  int32_t pendingDelay_ = 0;

  double phase_ = 0.25;
  float pitchEnv_ = 0.0f;
  float clickEnv_ = 0.0f;
  uint32_t noise_ = kNoiseSeed;

  AdsrEnvelope env_;
  BiquadFilter clickFilter_{48000.0};
  Saturation saturation_{48000.0};
  Panner panner_;
};

}  // namespace webdsp
