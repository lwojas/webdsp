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
  // Stereo position, -1 (full left) .. 0 (centre, default) .. +1 (full right). Applied after
  // the voice filter, before the bus sum; see dsp/panner.h and docs/panning.md.
  Pan = 6,
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
  // Instrument default pan, -1..+1 (default 0). Added to the voice's own pan (VoiceParam::Pan /
  // trigger pan) and clamped; read live like Gain. See docs/panning.md.
  Pan = 5,
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
  // Bus output position (balance), -1..+1, default 0. Applied after BusGain; see
  // dsp/panner.h and docs/panning.md.
  BusPan = 27,
  // Phaser (dsp/phaser.h). Rate Hz, depth 0..1, centre Hz, feedback -0.95..0.95, stage count
  // (even, 2..12), mix 0..1 (0 = bypass).
  PhaserRate = 28,
  PhaserDepth = 29,
  PhaserCenter = 30,
  PhaserFeedback = 31,
  PhaserStages = 32,
  PhaserMix = 33,
  // Bitcrusher (dsp/bitcrusher.h). Bit depth 1..16, sample-hold factor 1..64, mix 0..1.
  CrushBits = 34,
  CrushDownsample = 35,
  CrushMix = 36,
};

// Kind ids for synthesized instruments (instrument.h). Mirrored in src/runtime/types.ts once
// the TS bridge exists.
enum class InstrumentKind : int32_t {
  Kick = 0,
  Snare = 1,
  Clap = 2,
  Tom = 3,
  HiHat = 4,
  Bass = 5,
};

// Kick instrument controls (instruments/kick.h). This is the Kick's whole public surface;
// ranges/defaults/units live in its metadata table (kKickParams) and are queryable through the
// ABI, so a UI never hardcodes them. Separate id space from VoiceParam/NodeParam — ids are
// meaningful only relative to an InstrumentKind.
enum class KickParam : int32_t {
  Tune = 0,        // Hz   — resting fundamental the pitch settles on
  PitchDrop = 1,   // oct  — how far above Tune the pitch starts
  PitchDecay = 2,  // s    — how fast it falls to Tune
  Decay = 3,       // s    — body length
  Punch = 4,       // 0..1 — attack click/transient level
  Drive = 5,       // 0..1 — saturation amount
};

// Snare (instruments/snare.h).
enum class SnareParam : int32_t {
  Tune = 0,   // Hz   — body pitch
  Decay = 1,  // s    — overall length (~-40 dB time of the noise tail)
  Snap = 2,   // 0..1 — transient + brightness of the noise
  Noise = 3,  // 0..1 — noise vs tonal body balance
};

// Clap (instruments/clap.h).
enum class ClapParam : int32_t {
  Tune = 0,    // Hz   — centre of the noise band
  Decay = 1,   // s    — length of the final tail
  Spread = 2,  // 0..1 — time between the burst cluster's hits
  Snap = 3,    // 0..1 — resonance/sharpness of the bursts
};

// Tom (instruments/tom.h).
enum class TomParam : int32_t {
  Tune = 0,        // Hz   — resting fundamental
  PitchDrop = 1,   // oct  — how far above Tune the strike starts
  Decay = 2,       // s    — ring length
  Body = 3,        // 0..1 — membrane overtones + stick tick
  Damp = 4,        // 0..1 — damping: shorter ring, darker tone
};

// Hi-hat (instruments/hihat.h).
enum class HiHatParam : int32_t {
  Tune = 0,   // st   — semitone offset of the metallic cluster
  Decay = 1,  // s    — short = closed, long = open
  Metal = 2,  // 0..1 — noise -> inharmonic metallic cluster
  Tone = 3,   // 0..1 — brightness (band/high-pass position)
};

// Bass (instruments/bass.h).
enum class BassParam : int32_t {
  Tune = 0,       // Hz   — root; note pitch is semitones on top
  Body = 1,       // 0..1 — round/subby (sine + sub) <-> rich (saw)
  Bite = 2,       // 0..1 — onset drive/brightness and how much velocity changes timbre
  Cutoff = 3,     // Hz   — resting low-pass cutoff (keytracked)
  Resonance = 4,  // 0..1 — filter resonance (level-compensated)
  Attack = 5,     // s
  Decay = 6,      // s
  Sustain = 7,    // 0..1 — held level
  Release = 8,    // s
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
