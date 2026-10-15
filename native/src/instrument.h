#pragma once
#include <cstdint>
#include "params.h"

// The contract for a synthesized audio *source*, as opposed to a DSPNode (an effect that
// processes an existing signal in place). A sample Voice and an Instrument are siblings: both
// produce audio and are summed into a Bus by busId, after which the ordinary track/master FX
// chains apply. An instrument owns its own sound design — whatever oscillators, envelopes and
// shapers it uses internally are private; the application sees only noteOn/noteOff and a small
// table of named parameters. See ARCHITECTURE.md, "Instruments".
//
// One Instrument object is one *machine instance* (e.g. "the kick on track 3"), monophonic:
// retriggering restarts/chokes it, like drum hardware. A sequencer wanting two kicks overlapping
// creates two instances. This keeps the engine free of a second voice-allocation policy.
namespace webdsp {

// Self-description of one control. Ranges are enforced by the instrument (setParam clamps),
// so a UI can build a knob straight from this without knowing what the parameter does inside.
struct InstrumentParamInfo {
  int32_t id;
  const char* name;
  float min;
  float max;
  float def;
  const char* unit;  // "Hz", "s", "oct", "" for unitless 0..1
};

class Instrument {
 public:
  virtual ~Instrument() = default;

  virtual InstrumentKind kind() const = 0;
  virtual void configure(double sampleRate) = 0;

  // `velocity` 0..1+ scales the level; `delayFrames` makes a mid-block event start
  // sample-accurately (as Voice::trigger); `pan` -1..+1 as VoiceParam::Pan; `pitch` is a
  // semitone offset applied on top of the instrument's own Tune (0 = Tune as set), the same
  // convention as a sample event's `pitch`. Unpitched aspects of an instrument may ignore it.
  virtual void noteOn(float velocity, int32_t delayFrames, float pan, float pitch) = 0;
  // Ends the sound early with a short click-free release. Optional for one-shot drums.
  virtual void noteOff() = 0;
  // Unknown id: no-op. Value is clamped to the parameter's range; non-finite is ignored.
  virtual void setParam(int32_t param, float value) = 0;
  // Silences immediately and clears internal state.
  virtual void reset() = 0;

  // Renders `numFrames` into busChannels, *accumulating* (same convention as Voice::render).
  // `scratch` is caller-owned planar scratch, >= 2 channels. Returns true while still sounding.
  // Must not allocate.
  virtual bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
                      float* const* scratch) = 0;
  virtual bool isActive() const = 0;
};

}  // namespace webdsp
