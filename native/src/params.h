#pragma once
#include <cstdint>

// Mirrors src/runtime/types.ts VoiceParam / NodeParam. Manually kept in sync (see the
// comment on those TS enums); test/paramIds.test.ts pins the TS-side literal values.
namespace webdsp {

enum class VoiceParam : int32_t {
  Gain = 0,
  Rate = 1,
  FilterCutoff = 2,
  FilterResonance = 3,
  FilterMode = 4,
  FilterSlope = 5,  // dB/oct: 12 (default) or 24
};

// Sample-addressed parameters — a persistent property of the *sample itself* (SampleStore),
// not of any one playing voice. Applied once per render frame, multiplicatively with
// VoiceParam::Gain (see Voice::render), as the signal-level stage furthest upstream: it's the
// input-trim an application sets once per instrument/sample (e.g. to level-match a quiet kick
// against a hot snare) and leave alone, as opposed to VoiceParam::Gain, which a caller is
// expected to drive per trigger/event (velocity, a future per-cell level). Separate id space
// from VoiceParam/NodeParam for the same reason those two are separate from each other (see
// NodeParam's own doc comment) — a SampleParam and a VoiceParam that happen to share a numeric
// value don't mean the same thing.
enum class SampleParam : int32_t {
  Gain = 0,
  // Amplitude envelope of every voice playing this sample (see dsp/adsr_envelope.h and
  // ARCHITECTURE.md, "Envelopes"). Seconds, except sustain (0..1 level). Defaults: instant
  // attack, no decay, sustain 1, 10 ms release — i.e. no audible envelope.
  EnvAttack = 1,
  EnvDecay = 2,
  EnvSustain = 3,
  EnvRelease = 4,
};

enum class NodeParam : int32_t {
  DelayTime = 0,
  DelayFeedback = 1,
  DelayMix = 2,
  BusGain = 3,
  FilterCutoff = 4,
  FilterResonance = 5,
  FilterMode = 6,
  ReverbDecay = 7,
  ReverbDamping = 8,
  ReverbMix = 9,
  CompThreshold = 10,
  CompRatio = 11,
  CompAttack = 12,
  CompRelease = 13,
  CompKnee = 14,
  CompMakeup = 15,
  SatDrive = 16,
  SatAsymmetry = 17,
  SatOutputGain = 18,
  SatMix = 19,
  ChorusFlangerRate = 20,
  ChorusFlangerDepth = 21,
  ChorusFlangerDelay = 22,
  ChorusFlangerFeedback = 23,
  ChorusFlangerStereoPhase = 24,
  ChorusFlangerMix = 25,
  FilterSlope = 26,  // dB/oct: 12 (default) or 24
};

// Shared 0/1 values for VoiceParam::FilterMode / NodeParam::FilterMode. Not itself part of
// either enum's numeric contract (it's a value, not a param id), but kept alongside them
// since both filter instances (per-voice, per-bus) interpret it identically.
enum class FilterMode : int32_t {
  LowPass = 0,
  HighPass = 1,
};

// Internal representation of VoiceParam/NodeParam::FilterSlope's dB/oct value.
enum class FilterSlope : int32_t {
  Db12 = 12,
  Db24 = 24,
};

}  // namespace webdsp
