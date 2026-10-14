#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Stereo pan/balance stage shared by Voice (VoiceParam::Pan) and Bus (NodeParam::BusPan). Full
// reference: docs/panning.md.
//
// pan is -1 (full left) .. 0 (centre, the default) .. +1 (full right). Two laws, picked by the
// kind of signal being placed:
//
//  * Mono source (a 1-channel sample, already duplicated onto both output channels by
//    Voice::render): constant-power pan, normalised so centre is unity.
//        theta = (pan + 1) * pi/4;  gL = sqrt(2) * cos(theta);  gR = sqrt(2) * sin(theta)
//    gL^2 + gR^2 == 2 at every position, so loudness is constant across the sweep. Centre is
//    exactly 1/1 — the same output an unpanned mono sample produced before pan existed — so
//    adding this stage changes nothing for existing content. (The usual unnormalised law
//    would put centre at -3 dB and silently re-level every project.) Full left/right: the
//    far channel is exactly 0, the near one sqrt(2) (+3 dB).
//
//  * Stereo source (2-channel sample, or a whole bus): balance. The side being moved away from
//    is attenuated with cos(), the other stays at unity; the channels are not cross-mixed.
//        pan <= 0: gL = 1, gR = cos(-pan * pi/2);   pan > 0: gL = cos(pan * pi/2), gR = 1
//    Centre is unity, full left silences R entirely. A stereo image keeps its own width
//    instead of being collapsed toward one speaker, and nothing is ever boosted.
//
// Gains are smoothed (one-pole, ~5 ms) so changing pan while audio plays does not click. When
// the gains rest exactly at 1/1 (the default) the stage is skipped, so unpanned signals pay
// nothing and stay bit-identical. No allocation anywhere; trig is evaluated only when pan or
// the source kind changes, never per frame.
namespace webdsp {

class Panner {
 public:
  void configure(double sampleRate) {
    coef_ = 1.0 - std::exp(-1.0 / (kSmoothSeconds * sampleRate));
  }

  // Non-finite input is ignored (a malformed command must not poison render state); the
  // value is clamped to [-1, 1].
  void setPan(float pan) {
    if (!std::isfinite(pan)) return;
    pan_ = std::max(-1.0f, std::min(1.0f, pan));
    dirty_ = true;
  }
  float pan() const { return pan_; }

  // Jump straight to the target gains instead of gliding — used when a voice starts from
  // idle, so a new note never sweeps in from a stale position.
  void snap(bool monoSource) {
    updateTarget(monoSource);
    curL_ = tgtL_;
    curR_ = tgtR_;
  }

  // Places (inL, inR) into (outL, outR): accumulating (+=) when `accumulate`, otherwise
  // overwriting; outL/outR may alias inL/inR for in-place use with accumulate == false.
  void process(const float* inL, const float* inR, float* outL, float* outR, int32_t n,
               bool monoSource, bool accumulate) {
    updateTarget(monoSource);
    for (int32_t i = 0; i < n; i++) {
      curL_ += (tgtL_ - curL_) * coef_;
      curR_ += (tgtR_ - curR_) * coef_;
      if (std::fabs(tgtL_ - curL_) < kSnap) curL_ = tgtL_;
      if (std::fabs(tgtR_ - curR_) < kSnap) curR_ = tgtR_;
      const float l = inL[i] * static_cast<float>(curL_);
      const float r = inR[i] * static_cast<float>(curR_);
      if (accumulate) {
        outL[i] += l;
        outR[i] += r;
      } else {
        outL[i] = l;
        outR[i] = r;
      }
    }
  }

  // True when the stage is an exact pass-through and can be skipped.
  bool isUnity() const { return curL_ == 1.0 && curR_ == 1.0 && !(dirty_ && pan_ != 0.0f); }

 private:
  static constexpr double kSmoothSeconds = 0.005;
  static constexpr double kSnap = 1e-6;

  void updateTarget(bool monoSource) {
    if (!dirty_ && monoSource == lastMono_) return;
    dirty_ = false;
    lastMono_ = monoSource;
    double l = 1.0, r = 1.0;
    const double p = pan_;
    if (p != 0.0) {
      constexpr double kPi = 3.14159265358979323846;
      if (monoSource) {
        const double theta = (p + 1.0) * kPi / 4.0;
        l = std::sqrt(2.0) * std::cos(theta);
        r = std::sqrt(2.0) * std::sin(theta);
      } else if (p < 0.0) {
        r = std::cos(-p * kPi / 2.0);
      } else {
        l = std::cos(p * kPi / 2.0);
      }
      if (l < 1e-9) l = 0.0;  // cos(pi/2) is ~6e-17, not 0: make hard pan exactly silent
      if (r < 1e-9) r = 0.0;
    }
    tgtL_ = static_cast<double>(static_cast<float>(l));
    tgtR_ = static_cast<double>(static_cast<float>(r));
  }

  float pan_ = 0.0f;
  bool dirty_ = false;
  bool lastMono_ = false;
  // double: with float state the one-pole increment falls below an ulp ~1e-5 short of the
  // target and never reaches it, so exact unity/silence would never be restored.
  double tgtL_ = 1.0, tgtR_ = 1.0;
  double curL_ = 1.0, curR_ = 1.0;
  double coef_ = 0.004;
};

}  // namespace webdsp
