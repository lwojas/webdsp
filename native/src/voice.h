#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "dsp/adsr_envelope.h"
#include "dsp/biquad_filter.h"
#include "dsp/panner.h"
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

class Voice {
 public:
  void configure(double sampleRate) {
    sampleRate_ = sampleRate;
    filter_ = BiquadFilter(sampleRate);
    env_.configure(sampleRate);
    env_.reset();
    panner_.configure(sampleRate);
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
               int32_t delayFrames = 0, int32_t durationFrames = -1, float pan = 0.0f) {
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

    filter_.reset();
    // A fresh voice starts at its pan position; a stolen (still-active) voice glides there
    // over a few ms like any other pan change, so the retrigger itself never clicks.
    // A non-finite pan (e.g. an omitted ABI argument) means centre, never the voice's old pan.
    voicePan_ = std::isfinite(pan) ? pan : 0.0f;
    if (sample) panner_.setPan(effectivePan(sample));
    if (state_ == VoiceState::Idle && sample) panner_.snap(sample->channels == 1);
    const bool playable = sample_ && endFrame_ > startFrame_;
    // A fresh voice starts its envelope at 0. A stolen (still-active) voice keeps its
    // current envelope level and ramps from there, so the amplitude itself never jumps on a
    // retrigger. See ARCHITECTURE.md, "Envelopes".
    if (state_ == VoiceState::Idle || !playable) env_.reset();
    if (playable) env_.noteOn(sample_->envelope);
    state_ = playable ? VoiceState::Playing : VoiceState::Idle;
  }

  // note-off: starts the envelope's release; the voice stays alive (Releasing) until the
  // envelope reaches zero.
  void release() {
    if (state_ != VoiceState::Playing) return;
    state_ = VoiceState::Releasing;
    if (sample_) env_.noteOff(sample_->envelope);
  }

  void stop() {
    state_ = VoiceState::Idle;
    env_.reset();
  }
  // True while this voice may still read `sample`'s PCM. Engine checks this before freeing a
  // sample so no voice is left holding a dangling pointer.
  bool usesSample(const Sample* sample) const { return !isIdle() && sample_ == sample; }

  void setParam(int32_t param, float value) {
    if (param == static_cast<int32_t>(VoiceParam::Gain)) {
      gain_ = value;
    } else if (param == static_cast<int32_t>(VoiceParam::Rate)) {
      rate_ = value;
    } else if (param == static_cast<int32_t>(VoiceParam::Pan)) {
      if (std::isfinite(value)) voicePan_ = value;
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
        env_.noteOff(sample_->envelope);
      }
      if (durationFrames_ > 0) durationFrames_--;
      const float env = env_.next();
      for (int32_t c = 0; c < channels; c++) {
        const float* srcData = sample_->channelData[std::min(c, srcChannels - 1)];
        // sample_->gain (SampleParam::Gain, see sample_store.h) is the sample's own persistent
        // input trim — read fresh every frame straight off the Sample rather than cached on
        // the Voice, so editing an instrument's gain live updates every voice already playing
        // that sample, not just ones triggered afterward. gain_ (VoiceParam::Gain) stays this
        // voice's own per-trigger/event level (velocity today; a future per-cell level) — the
        // two multiply together rather than collapsing into one number, so a caller is never
        // forced to bake "the instrument's level" into every trigger() call by hand. Applied
        // before this voice's own filter and before it ever reaches the bus/track chain — see
        // ARCHITECTURE.md, "How DSP is composed".
        scratch[c][i] = cubicHermite(srcData, sample_->length, pos_) * gain_ * sample_->gain * env;
      }

      // The envelope is the voice's lifetime signal: once it has finished releasing the
      // voice is done (and only then — a releasing voice is never reclaimed early).
      if (state_ == VoiceState::Releasing && !env_.isActive()) {
        finished = true;
        framesRendered = i + 1;
        break;
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

    // Pan after the filter, before the bus sum (see docs/panning.md). A 1-channel sample was
    // duplicated onto both channels above, so it takes the mono (constant-power) law; a
    // 2-channel sample takes balance. Unity pan skips the stage and stays bit-identical.
    // Instrument pan is read live off the Sample (like gain), so editing it moves playing voices.
    panner_.setPan(effectivePan(sample_));
    if (channels >= 2 && !panner_.isUnity()) {
      panner_.process(scratch[0], scratch[1], busChannels[0], busChannels[1], numFrames,
                      srcChannels == 1, /*accumulate=*/true);
    } else {
      for (int32_t c = 0; c < channels; c++) {
        for (int32_t i = 0; i < numFrames; i++) busChannels[c][i] += scratch[c][i];
      }
    }

    if (finished) {
      state_ = VoiceState::Idle;
      env_.reset();
    }
    return !finished;
  }

  bool isIdle() const { return state_ == VoiceState::Idle; }
  int32_t id() const { return voiceId_; }
  int32_t busId() const { return busId_; }
  bool isReleasing() const { return state_ == VoiceState::Releasing; }
  // Includes sample_->gain so voice-stealing (VoiceManager, see ARCHITECTURE.md's "How voices
  // work") compares what's actually about to come out of render(), not just this voice's own
  // per-trigger gain — otherwise a loud sample with a quiet trigger gain could look stealable
  // ahead of a quiet sample triggered at full gain, the reverse of their real output levels.
  float amplitudeEstimate() const { return gain_ * (sample_ ? sample_->gain : 1.0f) * env_.level(); }

 private:
  // Voice pan + instrument default pan, clamped by Panner::setPan.
  float effectivePan(const Sample* s) const { return voicePan_ + s->pan; }

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

  AdsrEnvelope env_;
  int32_t pendingDelay_ = 0;
  int32_t durationFrames_ = -1;

  BiquadFilter filter_{48000.0};
  Panner panner_;
  float voicePan_ = 0.0f;
};

}  // namespace webdsp
