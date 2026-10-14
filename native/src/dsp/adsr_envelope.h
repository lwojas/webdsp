#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Reusable ADSR amplitude envelope — pure DSP, no knowledge of voices, samples, or the
// application-facing control model. Voice owns one today; a future instrument (VA kick's
// amp/pitch envelopes, a synth's filter envelope) can own as many as it needs, each with
// its own AdsrParams. See ARCHITECTURE.md, "Envelopes".
//
// Shape: linear segments in amplitude. Attack ramps level -> 1, decay ramps 1 -> sustain,
// sustain holds, release ramps (current level) -> 0 over exactly `release` seconds, so a
// release always reaches true zero and the owner gets a clear isActive() == false signal.
//
// Times <= 0 mean "instant" for that stage (a jump, not an error). Times round to the
// nearest frame, and any positive time takes at least one frame. No allocation, no branches on
// anything but the stage; safe for the audio thread.
namespace webdsp {

constexpr float kMaxEnvelopeSeconds = 30.0f;

// Control-rate parameters: seconds and a 0..1 level. Not per-sample coefficients — the
// envelope derives those itself when a stage starts, which is also what makes a parameter
// change mid-note safe (it applies at the next stage boundary; nothing is retroactively
// re-scaled). Defaults reproduce the engine's pre-ADSR behaviour: instant attack, full
// sustain, a short 10 ms click-free release.
struct AdsrParams {
  float attack = 0.0f;
  float decay = 0.0f;
  float sustain = 1.0f;
  float release = 0.01f;
};

class AdsrEnvelope {
 public:
  enum class Stage : int32_t { Idle, Attack, Decay, Sustain, Release };

  void configure(double sampleRate) { sampleRate_ = sampleRate; }

  void reset() {
    stage_ = Stage::Idle;
    level_ = 0.0f;
    step_ = 0.0f;
    framesLeft_ = 0;
    sustain_ = 1.0f;
  }

  // Starts (or retriggers) the envelope. Attack always ramps from the *current* level, so a
  // retrigger on a still-sounding envelope (voice steal, or note-on during release) carries
  // on from where it was instead of snapping to 0 or 1 — no discontinuity from the envelope
  // itself. From Idle the level is 0.
  void noteOn(const AdsrParams& p) {
    sustain_ = clamp01(p.sustain);
    attackSeconds_ = clampTime(p.attack);
    decaySeconds_ = clampTime(p.decay);
    const double frames = toFrames(attackSeconds_);
    if (frames < 1.0 || level_ >= 1.0f) {
      level_ = 1.0f;
      enterDecay();
    } else {
      step_ = (1.0f - level_) / static_cast<float>(frames);
      framesLeft_ = static_cast<int32_t>(frames);
      stage_ = Stage::Attack;
    }
  }

  // Begins the release from the current level (works from any stage, including mid-attack).
  // Release time is read here, so it can be changed live for notes already held. A repeated
  // noteOff while already releasing is a no-op.
  void noteOff(const AdsrParams& p) {
    if (stage_ == Stage::Idle || stage_ == Stage::Release) return;
    const double frames =
        toFrames(clampTime(p.release));
    if (frames < 1.0 || level_ <= 0.0f) {
      level_ = 0.0f;
      stage_ = Stage::Idle;
      return;
    }
    step_ = level_ / static_cast<float>(frames);
    framesLeft_ = static_cast<int32_t>(frames);
    stage_ = Stage::Release;
  }

  // Returns the envelope level to apply to this frame, then advances one frame. After the
  // final release frame isActive() turns false.
  float next() {
    const float out = level_;
    switch (stage_) {
      case Stage::Attack:
        level_ += step_;
        if (--framesLeft_ <= 0 || level_ >= 1.0f) {
          level_ = 1.0f;
          enterDecay();
        }
        break;
      case Stage::Decay:
        level_ -= step_;
        if (--framesLeft_ <= 0 || level_ <= sustain_) {
          level_ = sustain_;
          stage_ = Stage::Sustain;
        }
        break;
      case Stage::Release:
        level_ -= step_;
        if (--framesLeft_ <= 0 || level_ <= 0.0f) {
          level_ = 0.0f;
          stage_ = Stage::Idle;
        }
        break;
      case Stage::Sustain:
      case Stage::Idle:
        break;
    }
    return out;
  }

  bool isActive() const { return stage_ != Stage::Idle; }
  Stage stage() const { return stage_; }
  float level() const { return level_; }

  static AdsrParams sanitize(AdsrParams p) {
    p.attack = clampTime(p.attack);
    p.decay = clampTime(p.decay);
    p.sustain = clamp01(p.sustain);
    p.release = clampTime(p.release);
    return p;
  }

 private:
  static float clamp01(float v) { return std::isfinite(v) ? std::min(1.0f, std::max(0.0f, v)) : 0.0f; }
  static float clampTime(float v) {
    return std::isfinite(v) ? std::min(kMaxEnvelopeSeconds, std::max(0.0f, v)) : 0.0f;
  }

  // Nearest whole frame, at least 1 for any positive time (0 stays 0 = instant stage).
  double toFrames(float seconds) const {
    if (seconds <= 0.0f) return 0.0;
    return std::max(1.0, std::round(static_cast<double>(seconds) * sampleRate_));
  }

  void enterDecay() {
    const double frames = toFrames(decaySeconds_);
    if (frames < 1.0 || sustain_ >= 1.0f) {
      level_ = sustain_ >= 1.0f ? 1.0f : sustain_;
      stage_ = Stage::Sustain;
    } else {
      step_ = (1.0f - sustain_) / static_cast<float>(frames);
      framesLeft_ = static_cast<int32_t>(frames);
      stage_ = Stage::Decay;
    }
  }

  double sampleRate_ = 48000.0;
  Stage stage_ = Stage::Idle;
  float level_ = 0.0f;
  float step_ = 0.0f;
  // Frames left in the current ramp: the stage ends on this count (snapping to its exact
  // target), not on float accumulation, so stage lengths are exact at any duration.
  int32_t framesLeft_ = 0;
  float sustain_ = 1.0f;
  float attackSeconds_ = 0.0f;
  float decaySeconds_ = 0.0f;
};

}  // namespace webdsp
