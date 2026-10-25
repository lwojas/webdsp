#pragma once
#include <cstdint>
#include <cstring>
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

// How a parameter may be modulated by the (parameter-agnostic) LFO layer, declared by the synth
// that owns the parameter. Auto (the default for a table row that says nothing) = modulatable,
// domain inferred by resolveModDomain() below, so every new synth gets LFO modulation without any
// code; a synth opts a control OUT with None (on/off switches such as legato) and may pin
// Linear/Log explicitly. None = not a modulation target (a switch, a step).
// Linear: modulation moves the value in its own units, 100% = half the range either side.
// Log:    modulation moves it in octaves (value * 2^x), 100% = half the range, in octaves, either
//         side — the right domain for Hz-valued controls (cutoff, pitch). Needs min > 0.
// See modulation.h and docs/lfo-modulation.md.
enum class ModDomain : int32_t { None = 0, Linear = 1, Log = 2, Auto = 3 };

// Self-description of one control. Ranges are enforced by the instrument (setParam clamps),
// so a UI can build a knob straight from this without knowing what the parameter does inside.
struct InstrumentParamInfo {
  int32_t id;
  const char* name;
  float min;
  float max;
  float def;
  const char* unit;  // "Hz", "s", "oct", "" for unitless 0..1
  ModDomain mod = ModDomain::Auto;  // optional; omitted = modulatable, domain inferred
};

// The effective domain of a control (never Auto). Auto: stepped controls (unit "step") are not
// targets; Hz- and seconds-valued controls with a positive minimum are octave-domain (Log);
// everything else (unit-range, semitones, octaves, zero-based times) is Linear.
inline ModDomain resolveModDomain(const InstrumentParamInfo& p) {
  if (p.mod != ModDomain::Auto) return p.mod;
  if (p.unit && std::strcmp(p.unit, "step") == 0) return ModDomain::None;
  const bool timeOrFreq = p.unit && (std::strcmp(p.unit, "Hz") == 0 || std::strcmp(p.unit, "s") == 0);
  return timeOrFreq && p.min > 0.0f ? ModDomain::Log : ModDomain::Linear;
}

// The shared source-gain control every instrument kind has. It is NOT part of any kind's own
// table and no Instrument implements it: the Engine owns it per instance and applies it to the
// instrument's output before it is summed into its bus (so before the track's FX chain and the
// mixer fader). instrument_registry.h appends this row to every kind's catalogue, so it appears in
// getInstrumentParams() like any control, addressed by `kInstrumentGainParam`. dB, default 0 =
// unity (a sound is bit-identical until it is changed). Not an LFO target (ModDomain::None).
inline constexpr int32_t kInstrumentGainParam = 1000;
inline constexpr float kInstrumentGainMinDb = -24.0f;
inline constexpr float kInstrumentGainMaxDb = 12.0f;
inline constexpr InstrumentParamInfo kInstrumentGainInfo = {
    kInstrumentGainParam, "gain", kInstrumentGainMinDb, kInstrumentGainMaxDb, 0.0f, "dB", ModDomain::None};

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

  // The engine's beat grid, offered before every render() call: `frame` is the absolute frame of
  // the first sample about to be rendered; beat N sits at `anchorFrame + N * framesPerBeat`.
  // Instruments with tempo-aware behaviour (Pulse) derive their phase from it; everything else
  // ignores it. The engine knows a grid, not patterns or tracks. See ARCHITECTURE.md, "Transport".
  virtual void syncTransport(int64_t frame, double anchorFrame, double framesPerBeat) {
    (void)frame; (void)anchorFrame; (void)framesPerBeat;
  }

  // Renders `numFrames` into busChannels, *accumulating* (same convention as Voice::render).
  // `scratch` is caller-owned planar scratch, >= 2 channels. Returns true while still sounding.
  // Must not allocate.
  virtual bool render(float* const* busChannels, int32_t busChannelCount, int32_t numFrames,
                      float* const* scratch) = 0;
  virtual bool isActive() const = 0;
};

}  // namespace webdsp
