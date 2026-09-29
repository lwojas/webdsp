#pragma once
#include <algorithm>
#include <cmath>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Feedforward, log-domain, soft-knee compressor with a peak detector — the "smallest useful"
// conventional design per docs/effects-algorithm-survey.md, which compares this directly
// against feedback/RMS variants: Giannoulis, Massberg, Reiss, "Digital Dynamic Range
// Compressor Design — A Tutorial and Analysis" (JAES 60(6), 2012). No sidechain input (out of
// scope for this pass, per the ticket).
//
// Topology (the tutorial's canonical "peak detector -> gain computer" order, not the reverse):
// instantaneous peak level (dB) is first smoothed by a one-pole attack/release branching
// envelope follower (in the log domain) into a slowly-varying level estimate; *that* estimate
// is what feeds the static soft-knee gain-computer curve, converted to a linear multiplier
// (with makeup gain) and applied to every channel equally (stereo-linked via the max of the
// channels' instantaneous levels, so a bus compressor doesn't shift the stereo image).
// Smoothing must happen before the (nonlinear) gain computer, not after: feeding the curve a
// raw per-sample rectified sinusoid and only smoothing its output re-derives a new, distorted
// waveform shape every cycle instead of tracking the signal's actual envelope.
namespace webdsp {

class Compressor final : public DSPNode {
 public:
  explicit Compressor(double sampleRate) : sampleRate_(sampleRate) { recomputeCoeffs(); }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (bypassed_) return;
    for (int32_t i = 0; i < numFrames; i++) {
      float peak = 0.0f;
      for (int32_t c = 0; c < numChannels && c < kMaxDspChannels; c++) {
        peak = std::max(peak, std::fabs(channels[c][i]));
      }
      const float levelDb = 20.0f * std::log10(std::max(peak, kMinLevel));

      // Attack while the instantaneous level exceeds the current envelope (rising), release
      // while it's below (falling) — the standard branching peak-detector ballistic, applied
      // to the *level* so the gain computer below only ever sees a smooth signal.
      const float coeff = (levelDb > envelopeDb_) ? attackCoeff_ : releaseCoeff_;
      envelopeDb_ = coeff * envelopeDb_ + (1.0f - coeff) * levelDb;

      const float gainReductionDb = staticCurve(envelopeDb_) - envelopeDb_;  // always <= 0
      const float gainLin = std::pow(10.0f, (gainReductionDb + makeupGainDb_) / 20.0f);
      for (int32_t c = 0; c < numChannels && c < kMaxDspChannels; c++) {
        channels[c][i] *= gainLin;
      }
    }
  }

  void setParam(int32_t param, float value) override {
    if (param == static_cast<int32_t>(NodeParam::CompThreshold)) {
      thresholdDb_ = std::clamp(value, -60.0f, 0.0f);
    } else if (param == static_cast<int32_t>(NodeParam::CompRatio)) {
      ratio_ = std::clamp(value, 1.0f, 20.0f);
    } else if (param == static_cast<int32_t>(NodeParam::CompAttack)) {
      attackSeconds_ = std::clamp(value, 0.0001f, 1.0f);
      recomputeCoeffs();
    } else if (param == static_cast<int32_t>(NodeParam::CompRelease)) {
      releaseSeconds_ = std::clamp(value, 0.001f, 3.0f);
      recomputeCoeffs();
    } else if (param == static_cast<int32_t>(NodeParam::CompKnee)) {
      kneeDb_ = std::clamp(value, 0.0f, 24.0f);
    } else if (param == static_cast<int32_t>(NodeParam::CompMakeup)) {
      makeupGainDb_ = std::clamp(value, -24.0f, 24.0f);
    } else {
      return;  // unrecognized id: leave bypass state alone
    }
    bypassed_ = false;
  }

  void reset() override { envelopeDb_ = kSilenceDb; }

 private:
  float staticCurve(float levelDb) const {
    const float overshoot = levelDb - thresholdDb_;
    const float knee = std::max(kneeDb_, 1e-6f);  // guard the knee branch's division
    if (2.0f * overshoot < -knee) {
      return levelDb;  // below the knee: untouched
    } else if (2.0f * std::fabs(overshoot) <= knee) {
      const float t = overshoot + knee / 2.0f;
      return levelDb + (1.0f / ratio_ - 1.0f) * t * t / (2.0f * knee);
    } else {
      return thresholdDb_ + overshoot / ratio_;  // above the knee: full ratio
    }
  }

  void recomputeCoeffs() {
    attackCoeff_ = std::exp(-1.0 / (sampleRate_ * attackSeconds_));
    releaseCoeff_ = std::exp(-1.0 / (sampleRate_ * releaseSeconds_));
  }

  static constexpr float kMinLevel = 1e-9f;  // floor before log10, ~ -180 dB
  static constexpr float kSilenceDb = -180.0f;  // 20*log10(kMinLevel) — the envelope's rest value

  double sampleRate_;
  float thresholdDb_ = -18.0f;    // NodeParam::CompThreshold
  float ratio_ = 4.0f;            // NodeParam::CompRatio
  float attackSeconds_ = 0.01f;   // NodeParam::CompAttack
  float releaseSeconds_ = 0.15f;  // NodeParam::CompRelease
  float kneeDb_ = 6.0f;           // NodeParam::CompKnee
  float makeupGainDb_ = 0.0f;     // NodeParam::CompMakeup
  bool bypassed_ = true;          // no param set yet => pass-through, matches BiquadFilter

  float attackCoeff_ = 0.0;
  float releaseCoeff_ = 0.0;
  float envelopeDb_ = kSilenceDb;  // current smoothed peak level estimate, dB
};

}  // namespace webdsp
