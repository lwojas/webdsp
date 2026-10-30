#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

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
constexpr float kRestartFadeSeconds = 0.002f;
constexpr int32_t kHeldMaxFrames = 2048;

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

// A per-trigger override of a sample's envelope. A NaN field keeps the sample's own value, so an
// omitted ABI argument (JS passes NaN for an omitted float) inherits and the default is the old behaviour.
struct AdsrOverride {
  float attack = std::numeric_limits<float>::quiet_NaN();
  float decay = std::numeric_limits<float>::quiet_NaN();
  float sustain = std::numeric_limits<float>::quiet_NaN();
  float release = std::numeric_limits<float>::quiet_NaN();

  AdsrParams applyTo(const AdsrParams& base) const {
    AdsrParams p = base;
    if (std::isfinite(attack)) p.attack = attack;
    if (std::isfinite(decay)) p.decay = decay;
    if (std::isfinite(sustain)) p.sustain = sustain;
    if (std::isfinite(release)) p.release = release;
    return p;
  }
};

class AdsrEnvelope {
 public:
  enum class Stage : int32_t { Idle, Attack, Decay, Sustain, Release, Fade, Held };

  void configure(double sampleRate) { sampleRate_ = sampleRate; }

  void reset() {
    stage_ = Stage::Idle;
    level_ = 0.0f;
    step_ = 0.0f;
    framesLeft_ = 0;
    sustain_ = 1.0f;
    duckOnly_ = false;
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

  // Retrigger for a monophonic synth: unlike noteOn(), a still-sounding envelope does not carry
  // its level into the new attack (that skips the attack stage whenever notes sit back to
  // back). The level instead ramps to zero over kRestartFadeSeconds (declicked, ~2 ms), then
  // the new note's attack/decay run from silence. From Idle or zero it is exactly noteOn().
  void noteOnRestart(const AdsrParams& p) {
    if (stage_ == Stage::Idle || level_ <= 0.0f) {
      level_ = 0.0f;
      noteOn(p);
      return;
    }
    pending_ = p;
    duckOnly_ = false;
    const double frames = std::max(1.0, std::round(static_cast<double>(kRestartFadeSeconds) * sampleRate_));
    step_ = level_ / static_cast<float>(frames);
    framesLeft_ = static_cast<int32_t>(frames);
    stage_ = Stage::Fade;
  }

  // Fades the level to zero so that it arrives after `frames` frames, then holds silence (Stage::Held)
  // until the next noteOnRestart() starts the new note's attack from zero on its own frame. The
  // engine calls this ahead of a scheduled retrigger it knows the frame of, so the declick happens
  // before the note instead of delaying it. Nothing to do when silent. A Held envelope that is never
  // retriggered ends after kHeldMaxFrames (a cancelled note must not leave a silent voice alive).
  void duck(int32_t frames) {
    if (stage_ == Stage::Idle || stage_ == Stage::Fade || stage_ == Stage::Held || level_ <= 0.0f) return;
    const int32_t n = std::max(1, frames);
    step_ = level_ / static_cast<float>(n);
    framesLeft_ = n;
    duckOnly_ = true;
    stage_ = Stage::Fade;
  }

  // Begins the release from the current level (works from any stage, including mid-attack).
  // Release time is read here, so it can be changed live for notes already held. A repeated
  // noteOff while already releasing is a no-op.
  void noteOff(const AdsrParams& p) {
    if (stage_ == Stage::Idle || stage_ == Stage::Release) return;
    duckOnly_ = false;
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
      case Stage::Fade:
        level_ -= step_;
        if (--framesLeft_ <= 0 || level_ <= 0.0f) {
          level_ = 0.0f;
          if (duckOnly_) {
            duckOnly_ = false;
            stage_ = Stage::Held;
            framesLeft_ = kHeldMaxFrames;
          } else {
            noteOn(pending_);
          }
        }
        break;
      case Stage::Held:
        if (--framesLeft_ <= 0) stage_ = Stage::Idle;
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
  // Frames of a restart fade still to render (0 outside Stage::Fade): after exactly this many
  // next() calls the new note's attack begins from silence.
  int32_t fadeFramesLeft() const { return stage_ == Stage::Fade ? framesLeft_ : 0; }

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
  AdsrParams pending_;
  Stage stage_ = Stage::Idle;
  float level_ = 0.0f;
  float step_ = 0.0f;
  // Frames left in the current ramp: the stage ends on this count (snapping to its exact
  // target), not on float accumulation, so stage lengths are exact at any duration.
  int32_t framesLeft_ = 0;
  bool duckOnly_ = false;  // the Fade in progress came from duck(): no pending note follows it
  float sustain_ = 1.0f;
  float attackSeconds_ = 0.0f;
  float decaySeconds_ = 0.0f;
};

}  // namespace webdsp
