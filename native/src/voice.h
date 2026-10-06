#pragma once
#include <algorithm>
#include <cstdint>
#include "dsp/biquad_filter.h"
#include "dsp_node.h"
#include "params.h"
#include "resampler.h"
#include "sample_store.h"

// A Voice is one playing instance of a sample. The engine owns a pool of these (see
// VoiceManager) — it has no notion of "pads" or "channels", only voices that can be
// triggered, parameterized in real time, released, and reused. See ARCHITECTURE.md,
// "Voices, not channels".
namespace webdsp {

enum class VoiceState : int32_t { Idle, Playing, Releasing };

// Short linear fade applied on release()/steal so the signal never clicks. Not an ADSR
// envelope (not requested) — just enough to make retriggering and voice stealing silent.
constexpr float kReleaseSeconds = 0.01f;

class Voice {
 public:
  void configure(double sampleRate) {
    sampleRate_ = sampleRate;
    filter_ = BiquadFilter(sampleRate);
  }

  // `delayFrames` lets a scheduled event that lands mid-block start exactly on time: the
  // voice renders silence for the first `delayFrames` samples of its next render() call,
  // then begins playback, giving sample-accurate timing within a render quantum rather than
  // being quantized to block boundaries (~2.7ms at 128 frames/48kHz). See
  // ARCHITECTURE.md, "Scheduling".
  // `durationFrames`, when >= 0, auto-releases the voice (same envelope taper as an
  // explicit release() call) after that many frames of actual playback — i.e. counted from
  // when `delayFrames` has elapsed and the sample audibly starts, not from the trigger
  // call. -1 (default) means no auto-release: the voice plays until it explicitly
  // release()s/stop()s or the sample itself ends (matching pre-existing behavior). See
  // ARCHITECTURE.md, "Scheduling" — this is what lets a sequencer express note duration
  // without a second, separately-timed release message.
  void trigger(int32_t voiceId, const Sample* sample, int32_t busId, float gain, float rate,
               int32_t startFrame, int32_t endFrame, bool loop, bool reverse,
               int32_t delayFrames = 0, int32_t durationFrames = -1) {
    voiceId_ = voiceId;
    sample_ = sample;
    busId_ = busId;
    gain_ = gain;
    rate_ = rate;
    loop_ = loop;
    reverse_ = reverse;
    pendingDelay_ = std::max(0, delayFrames);
    durationFrames_ = durationFrames;

    const int32_t maxLen = sample ? sample->length : 0;
    startFrame_ = std::max(0, std::min(startFrame, maxLen));
    endFrame_ = endFrame < 0 ? maxLen : std::max(startFrame_, std::min(endFrame, maxLen));
    pos_ = reverse_ ? static_cast<double>(endFrame_) - 1.0 : static_cast<double>(startFrame_);

    envelope_ = 1.0f;
    envelopeStep_ = 0.0f;
    filter_.reset();
    state_ = (sample_ && endFrame_ > startFrame_) ? VoiceState::Playing : VoiceState::Idle;
  }

  void release() {
    if (state_ != VoiceState::Playing) return;
    state_ = VoiceState::Releasing;
    envelopeStep_ = 1.0f / static_cast<float>(sampleRate_ * kReleaseSeconds);
  }

  void stop() { state_ = VoiceState::Idle; }
  // True while this voice may still read `sample`'s PCM. Engine checks this before freeing a
  // sample so no voice is left holding a dangling pointer.
  bool usesSample(const Sample* sample) const { return !isIdle() && sample_ == sample; }

  void setParam(int32_t param, float value) {
    if (param == static_cast<int32_t>(VoiceParam::Gain)) {
      gain_ = value;
    } else if (param == static_cast<int32_t>(VoiceParam::Rate)) {
      rate_ = value;
    } else {
      filter_.setParam(param, value);
    }
  }

  // Renders up to `numFrames` into busChannels (accumulated, not overwritten — the Engine
  // zeroes each bus buffer once per block before any voice renders into it). `scratch` is a
  // caller-owned, per-block-size scratch buffer (avoids a per-voice, per-call allocation).
  // Returns false once the voice has finished and can be reclaimed by the VoiceManager.
  bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
              float* const* scratch) {
    if (state_ == VoiceState::Idle || !sample_) return false;

    const int32_t channels = std::min(busChannelCount, kMaxDspChannels);
    const int32_t srcChannels = sample_->channels;
    const double step = rate_ * (reverse_ ? -1.0 : 1.0);
    bool finished = false;
    int32_t framesRendered = numFrames;

    for (int32_t i = 0; i < numFrames; i++) {
      if (pendingDelay_ > 0) {
        for (int32_t c = 0; c < channels; c++) scratch[c][i] = 0.0f;
        pendingDelay_--;
        continue;
      }
      if (state_ == VoiceState::Playing && durationFrames_ == 0) {
        state_ = VoiceState::Releasing;
        envelopeStep_ = 1.0f / static_cast<float>(sampleRate_ * kReleaseSeconds);
      }
      if (durationFrames_ > 0) durationFrames_--;
      for (int32_t c = 0; c < channels; c++) {
        const float* srcData = sample_->channelData[std::min(c, srcChannels - 1)];
        scratch[c][i] = cubicHermite(srcData, sample_->length, pos_) * gain_ * envelope_;
      }

      if (state_ == VoiceState::Releasing) {
        envelope_ -= envelopeStep_;
        if (envelope_ <= 0.0f) {
          finished = true;
          framesRendered = i + 1;
          break;
        }
      }

      pos_ += step;
      if (reverse_) {
        if (pos_ < startFrame_) {
          if (loop_) {
            pos_ = static_cast<double>(endFrame_) - (startFrame_ - pos_);
          } else {
            finished = true;
            framesRendered = i + 1;
          }
        }
      } else {
        if (pos_ >= endFrame_) {
          if (loop_) {
            pos_ = static_cast<double>(startFrame_) + (pos_ - endFrame_);
          } else {
            finished = true;
            framesRendered = i + 1;
          }
        }
      }
      if (finished) break;
    }

    for (int32_t c = 0; c < channels; c++) {
      for (int32_t i = framesRendered; i < numFrames; i++) scratch[c][i] = 0.0f;
    }

    filter_.process(scratch, channels, numFrames);

    for (int32_t c = 0; c < channels; c++) {
      for (int32_t i = 0; i < numFrames; i++) busChannels[c][i] += scratch[c][i];
    }

    if (finished) state_ = VoiceState::Idle;
    return !finished;
  }

  bool isIdle() const { return state_ == VoiceState::Idle; }
  int32_t id() const { return voiceId_; }
  int32_t busId() const { return busId_; }
  bool isReleasing() const { return state_ == VoiceState::Releasing; }
  float amplitudeEstimate() const { return gain_ * envelope_; }

 private:
  int32_t voiceId_ = -1;
  VoiceState state_ = VoiceState::Idle;
  const Sample* sample_ = nullptr;
  int32_t busId_ = 0;
  double sampleRate_ = 48000.0;

  double pos_ = 0.0;
  double rate_ = 1.0;
  float gain_ = 1.0f;
  int32_t startFrame_ = 0;
  int32_t endFrame_ = 0;
  bool loop_ = false;
  bool reverse_ = false;

  float envelope_ = 1.0f;
  float envelopeStep_ = 0.0f;
  int32_t pendingDelay_ = 0;
  int32_t durationFrames_ = -1;

  BiquadFilter filter_{48000.0};
};

}  // namespace webdsp
