#pragma once
#include <algorithm>
#include <cmath>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Feedforward, log-domain, soft-knee compressor — the "smallest useful" conventional design per
// docs/effects-algorithm-survey.md (Giannoulis, Massberg, Reiss, "Digital Dynamic Range Compressor
// Design — A Tutorial and Analysis", JAES 60(6), 2012). With CompSidechain set to a bus id the detector
// reads that bus's block (handed over by the engine through setKey()) instead of this node's own input,
// while the gain reduction is still applied to the node's own signal.
//
// Topology (see docs/compressor-node.md, "Detector and ballistics", for why it changed):
//   max |x| over the channels (stereo-linked: the image never shifts)
//     -> peak follower in the LINEAR domain (instant attack, short fixed decay that bridges the
//        zero crossings of a rectified low-frequency wave)
//     -> dB -> static soft-knee curve -> target gain reduction (dB, <= 0)
//     -> attack/release one-pole on the GAIN REDUCTION itself (attack while it deepens, release
//        while it recovers)
//     -> + makeup -> linear gain, applied to every channel.
// Smoothing the gain reduction (not the level) is what makes Attack and Release mean what the
// controls say, and tracking the true peak is what makes Threshold mean "peaks above this level
// are reduced", so Threshold -> Ratio -> Gain reduction is predictable. Makeup is only ever added
// after the gain reduction is computed: it can never change how much compression happens.
namespace webdsp {

class Compressor final : public DSPNode {
 public:
  explicit Compressor(double sampleRate) : sampleRate_(sampleRate) { recomputeCoeffs(); }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    if (bypassed_) return;
    const int32_t nc = std::min(numChannels, kMaxDspChannels);
    for (int32_t i = 0; i < numFrames; i++) {
      float peak = 0.0f;
      float own = 0.0f;
      for (int32_t c = 0; c < nc; c++) own = std::max(own, std::fabs(channels[c][i]));
      if (keyBus_ > 0) {
        // Sidechain: detect on the key bus. No key block (the bus is asleep) is silence, so the gain recovers.
        for (int32_t c = 0; c < keyChannels_ && c < kMaxDspChannels; c++) {
          peak = std::max(peak, std::fabs(key_[c][i]));
        }
      } else {
        peak = own;
      }
      // Peak follower: jumps to a new peak at once, otherwise decays with a short fixed constant.
      detector_ = std::max(peak, detector_ * detectorDecay_);
      const float levelDb = 20.0f * std::log10(std::max(detector_, kMinLevel));

      const float targetDb = staticCurve(levelDb) - levelDb;  // <= 0
      // Attack while the reduction is deepening, release while it recovers.
      const float coeff = (targetDb < grDb_) ? attackCoeff_ : releaseCoeff_;
      grDb_ = coeff * grDb_ + (1.0f - coeff) * targetDb;

      const float gainLin = std::pow(10.0f, (grDb_ + makeupGainDb_) / 20.0f);
      float out = 0.0f;
      for (int32_t c = 0; c < nc; c++) {
        channels[c][i] *= gainLin;
        out = std::max(out, std::fabs(channels[c][i]));
      }
      meterIn_ = std::max(meterIn_, own);
      meterOut_ = std::max(meterOut_, out);
      meterGrDb_ = std::min(meterGrDb_, grDb_);
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
    } else if (param == static_cast<int32_t>(NodeParam::CompSidechain)) {
      if (!std::isfinite(value)) return;
      keyBus_ = static_cast<int32_t>(std::clamp(std::lround(value), 0L, 64L));
      if (keyBus_ == 0) keyChannels_ = 0;
    } else {
      return;  // unrecognized id: leave bypass state alone
    }
    bypassed_ = false;
  }

  void reset() override {
    detector_ = 0.0f;
    grDb_ = 0.0f;
    meterGrDb_ = 0.0f;
    meterIn_ = meterOut_ = 0.0f;
  }

  // Display metering, read-and-cleared by the engine once per block: the peak at this node's input and
  // output (linear) and the deepest gain reduction (dB, <= 0, before makeup) since the previous call.
  // False when the node is bypassed (no param set yet), so it does not count as a compressor.
  bool takeMeter(float* inPeak, float* grDb, float* outPeak) override {
    if (bypassed_) return false;
    *inPeak = meterIn_;
    *grDb = meterGrDb_;
    *outPeak = meterOut_;
    meterIn_ = meterOut_ = 0.0f;
    meterGrDb_ = grDb_;  // the next interval starts from where the gain is now, not from 0
    return true;
  }

  int32_t keyBus() const override { return keyBus_; }
  // `channels` must stay valid for the whole block; null or 0 channels means the key bus rendered nothing.
  void setKey(const float* const* channels, int32_t numChannels) override {
    key_ = channels;
    keyChannels_ = channels ? numChannels : 0;
  }

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
    detectorDecay_ = std::exp(-1.0 / (sampleRate_ * kDetectorDecaySeconds));
  }

  static constexpr float kMinLevel = 1e-9f;  // floor before log10, ~ -180 dB
  // Peak follower decay. Long enough to hold a 40-50 Hz wave's level through its zero crossings (so the
  // gain does not ripple at twice the bass frequency), short enough to stay out of Attack/Release's way.
  static constexpr double kDetectorDecaySeconds = 0.02;

  double sampleRate_;
  float thresholdDb_ = -18.0f;    // NodeParam::CompThreshold
  float ratio_ = 4.0f;            // NodeParam::CompRatio
  float attackSeconds_ = 0.01f;   // NodeParam::CompAttack
  float releaseSeconds_ = 0.15f;  // NodeParam::CompRelease
  float kneeDb_ = 6.0f;           // NodeParam::CompKnee
  float makeupGainDb_ = 0.0f;     // NodeParam::CompMakeup
  int32_t keyBus_ = 0;            // NodeParam::CompSidechain: 0 = detect on the own input
  const float* const* key_ = nullptr;  // the key bus's block, valid for one process() call
  int32_t keyChannels_ = 0;
  bool bypassed_ = true;          // no param set yet => pass-through, matches BiquadFilter

  float attackCoeff_ = 0.0;
  float releaseCoeff_ = 0.0;
  float detectorDecay_ = 0.0;
  float detector_ = 0.0f;  // linear peak follower
  float grDb_ = 0.0f;      // smoothed gain reduction, dB (<= 0), before makeup
  float meterIn_ = 0.0f, meterOut_ = 0.0f, meterGrDb_ = 0.0f;
};

}  // namespace webdsp
