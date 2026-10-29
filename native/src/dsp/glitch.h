#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../dsp_node.h"
#include "../params.h"
#include "biquad_filter.h"  // kMaxDspChannels

// Glitch: a performance effect built on one ring buffer that always records the input. Three
// modes, chosen with `mode`, each a different way of reading that buffer (or the clock) while the
// effect is engaged. All of them are tempo-synchronised: `division` is a musical length, not a
// time (setTempo, fed by FxChain from the engine's beat grid), so they follow BPM by themselves.
//
//   Repeat  captures the last `division` of audio the moment the effect engages, then loops it
//           (recording is frozen so the capture cannot be overwritten). `speed` reads the loop
//           slower, faster or backwards. A division change while engaged shortens or lengthens the
//           loop at its next wrap, always keeping the most recent slice: an accelerating roll.
//   Tape    reads the live signal back through the buffer at `speed` (1 = untouched, 0 = stopped,
//           negative = backwards). The read position starts at the write position, so engaging is
//           seamless; slowing it down lets the delay grow like a tape machine losing speed. The
//           output fades out as speed approaches 0 (a stopped tape is silence, not DC).
//   Gate    chops the signal with a `division`-long square: open for the first half, closed for the
//           second, with short slews. The gate restarts open when the effect engages.
//
// `mix` is the effect's dry/wet (Repeat, Tape) or depth (Gate), smoothed over ~8 ms so engaging and
// releasing never click. mix = 0 is an exact bypass: the node leaves the audio untouched (it still
// records, so Repeat and Tape have history the instant they are engaged). Engaging is the 0 -> >0
// edge of `mix`; the mode and division in force at that moment are the ones used (a mode change
// while engaged waits for the next engage; a division change is followed live, see Repeat).
//
// Stereo: one ring per channel, one shared read/write position, so the image is preserved.
namespace webdsp {

// Division lengths in sixteenth notes. Index = the GlitchDivision value.
inline constexpr int32_t kGlitchDivisionCount = 6;
inline constexpr double kGlitchSixteenths[kGlitchDivisionCount] = {
    8,     // 1/2
    4,     // 1/4
    2,     // 1/8
    1,     // 1/16
    0.5,   // 1/32
    0.25,  // 1/64
};

enum class GlitchMode : int32_t { Repeat = 0, Tape = 1, Gate = 2 };

class Glitch final : public DSPNode {
 public:
  // How much history the buffer holds. A loop longer than this at a slow tempo is halved until it fits.
  static constexpr double kMaxSeconds = 4.0;

  explicit Glitch(double sampleRate) {
    capacity_ = static_cast<int64_t>(sampleRate * kMaxSeconds) + 4;
    for (int32_t c = 0; c < kMaxDspChannels; c++) ring_[c].assign(static_cast<size_t>(capacity_), 0.0f);
    mixSmooth_ = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * 0.008));
    gateSmooth_ = 1.0f - std::exp(-1.0f / static_cast<float>(sampleRate * 0.0015));
    fade_ = std::max<double>(8.0, sampleRate * 0.0015);
    framesPerBeat_ = sampleRate * 0.5;  // 120 BPM until the engine says otherwise
  }

  void setTempo(double framesPerBeat) override {
    if (framesPerBeat > 0.0) framesPerBeat_ = framesPerBeat;
  }

  void setParam(int32_t param, float value) override {
    if (!std::isfinite(value)) return;
    switch (static_cast<NodeParam>(param)) {
      case NodeParam::GlitchMix: {
        const float m = std::min(1.0f, std::max(0.0f, value));
        if (mixTarget_ <= 0.0f && m > 0.0f) pendingEngage_ = true;
        mixTarget_ = m;
        break;
      }
      case NodeParam::GlitchMode:
        mode_ = static_cast<int32_t>(std::lround(std::min(2.0f, std::max(0.0f, value))));
        break;
      case NodeParam::GlitchDivision:
        division_ = static_cast<int32_t>(std::lround(std::min(static_cast<float>(kGlitchDivisionCount - 1), std::max(0.0f, value))));
        break;
      case NodeParam::GlitchSpeed:
        speed_ = std::min(2.0f, std::max(-1.0f, value));
        break;
      default:
        break;
    }
  }

  void reset() override {
    for (int32_t c = 0; c < kMaxDspChannels; c++) std::fill(ring_[c].begin(), ring_[c].end(), 0.0f);
    written_ = 0;
    engaged_ = false;
    pendingEngage_ = mixTarget_ > 0.0f;
    mixSmoothed_ = 0.0f;
    gate_ = 1.0f;
  }

  void process(float* const* channels, int32_t numChannels, int32_t numFrames) override {
    const int32_t nc = std::min(numChannels, kMaxDspChannels);
    if (pendingEngage_) {
      engage();
      pendingEngage_ = false;
    }
    // Idle: an exact bypass that still records (the capture and the tape read need recent history).
    if (!engaged_ && mixTarget_ <= 0.0f && mixSmoothed_ <= 1e-6f) {
      record(channels, nc, numFrames);
      return;
    }
    const double len = loopFrames();
    for (int32_t i = 0; i < numFrames; i++) {
      mixSmoothed_ += (mixTarget_ - mixSmoothed_) * mixSmooth_;
      const float m = mixSmoothed_;
      float dry[kMaxDspChannels];
      for (int32_t c = 0; c < nc; c++) dry[c] = channels[c][i];
      float wet[kMaxDspChannels] = {};

      if (active_ == static_cast<int32_t>(GlitchMode::Repeat)) {
        // recording is frozen: the dry signal is not stored while a loop is held
        if (pos_ >= loopLen_) {
          pos_ = std::fmod(pos_, std::max(1.0, loopLen_));
          loopLen_ = len;  // a new division takes effect at the wrap
        } else if (pos_ < 0.0) {
          pos_ += loopLen_;
          loopLen_ = len;
        }
        const double w = std::min(1.0, std::min(pos_, loopLen_ - pos_) / fade_);
        const double at = static_cast<double>(freeze_) - loopLen_ + pos_;
        for (int32_t c = 0; c < nc; c++) wet[c] = static_cast<float>(read(c, at) * w);
        pos_ += speed_;
      } else if (active_ == static_cast<int32_t>(GlitchMode::Tape)) {
        for (int32_t c = 0; c < nc; c++) ring_[c][index(written_)] = dry[c];
        // the newest stored sample is `written_`; the read position may not pass it nor fall off the buffer
        double newest = static_cast<double>(written_);
        readPos_ += std::min(1.0f, speed_);
        readPos_ = std::min(newest, std::max(newest - static_cast<double>(capacity_ - 4), readPos_));
        // a stopped tape is silence; so is a reverse read that has run out of history
        const bool outOfHistory = newest - readPos_ >= static_cast<double>(capacity_ - 8);
        const float g = outOfHistory ? 0.0f : std::min(1.0f, std::fabs(speed_) / 0.1f);
        for (int32_t c = 0; c < nc; c++) wet[c] = static_cast<float>(read(c, readPos_) * g);
        written_++;
      } else {  // Gate
        for (int32_t c = 0; c < nc; c++) ring_[c][index(written_)] = dry[c];
        written_++;
        gatePhase_ += 1.0 / std::max(2.0, len);
        gatePhase_ -= std::floor(gatePhase_);
        const float target = gatePhase_ < 0.5 ? 1.0f : 0.0f;
        gate_ += (target - gate_) * gateSmooth_;
      }

      for (int32_t c = 0; c < nc; c++) {
        channels[c][i] = active_ == static_cast<int32_t>(GlitchMode::Gate)
                             ? dry[c] * (1.0f - m + m * gate_)
                             : dry[c] * (1.0f - m) + wet[c] * m;
      }
    }
    if (mixTarget_ <= 0.0f && mixSmoothed_ <= 1e-4f) {
      mixSmoothed_ = 0.0f;
      engaged_ = false;
      gate_ = 1.0f;
    }
  }

 private:
  int64_t index(int64_t absolute) const {
    const int64_t r = absolute % capacity_;
    return r < 0 ? r + capacity_ : r;
  }

  // Linear interpolation at an absolute (unwrapped) position.
  float read(int32_t c, double position) const {
    const double fl = std::floor(position);
    const float f = static_cast<float>(position - fl);
    const int64_t i = static_cast<int64_t>(fl);
    return ring_[c][index(i)] * (1.0f - f) + ring_[c][index(i + 1)] * f;
  }

  void record(float* const* channels, int32_t nc, int32_t numFrames) {
    for (int32_t i = 0; i < numFrames; i++) {
      for (int32_t c = 0; c < nc; c++) ring_[c][index(written_)] = channels[c][i];
      written_++;
    }
  }

  // The loop or gate length of the current division, in frames, short enough for the buffer.
  double loopFrames() const {
    double frames = kGlitchSixteenths[division_] * framesPerBeat_ / 4.0;
    const double limit = static_cast<double>(capacity_ - 8);
    while (frames > limit) frames *= 0.5;
    return std::max(8.0, frames);
  }

  void engage() {
    active_ = mode_;
    engaged_ = true;
    if (active_ == static_cast<int32_t>(GlitchMode::Repeat)) {
      loopLen_ = loopFrames();
      freeze_ = written_;  // the loop is the slice [freeze_ - loopLen_, freeze_)
      pos_ = 0.0;
    } else if (active_ == static_cast<int32_t>(GlitchMode::Tape)) {
      readPos_ = static_cast<double>(written_);
    } else {
      gatePhase_ = 0.0;
      gate_ = 1.0f;
    }
  }

  int64_t capacity_ = 0;
  std::vector<float> ring_[kMaxDspChannels];
  int64_t written_ = 0;  // absolute count of frames stored; the next one goes to index(written_)
  double framesPerBeat_ = 24000.0;

  float mixTarget_ = 0.0f;
  float mixSmoothed_ = 0.0f;
  float mixSmooth_ = 0.0f;
  float gateSmooth_ = 0.0f;
  double fade_ = 72.0;
  int32_t mode_ = 0;
  int32_t division_ = 1;
  float speed_ = 1.0f;

  bool pendingEngage_ = false;
  bool engaged_ = false;
  int32_t active_ = 0;  // the mode in force since the last engage
  int64_t freeze_ = 0;
  double loopLen_ = 0.0;
  double pos_ = 0.0;
  double readPos_ = 0.0;
  double gatePhase_ = 0.0;
  float gate_ = 1.0f;
};

}  // namespace webdsp
