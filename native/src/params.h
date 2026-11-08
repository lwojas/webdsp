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
  // Bus output destination (engine.h): busId a track bus sums into after its own chain; 0 =
  // MASTER_BUS (default). Rejected, leaving the route unchanged, for self/cyclic/unknown targets.
  BusOutput = 37,
  // Master limiter (dsp/limiter.h), addressed to MASTER_BUS only; ignored on track buses.
  // Enable 0/1 (default 1), ceiling dBFS -24..0 (default -0.3), release ms 5..2000 (default 100).
  LimiterEnable = 38,
  LimiterCeiling = 39,
  LimiterRelease = 40,
  // Utility FX (dsp/utility.h): gain in dB, -60 (silence) .. +12, default 0 (unity).
  UtilityGain = 41,
  // Bus send (engine.h): a second, independent output edge from a track bus, tapped after its
  // chain, gain and pan (post-fader). BusSendTarget is the destination track busId (0 = no send,
  // default; self/cyclic/unknown rejected, old send kept). BusSendAmount is the linear level of
  // that tap, 0..1 (default 1). BusMainGain scales only the main route (BusOutput), after the
  // send tap, default 1: a host's solo gates this without cutting the send.
  BusSendTarget = 42,
  BusSendAmount = 43,
  BusMainGain = 44,
  // Dub Delay FX (dsp/dub_delay.h): tempo-synced feedback delay. Division is a step index
  // (0 1/16, 1 1/8, 2 1/8 dotted, 3 1/4, 4 1/4 dotted, 5 1/2, 6-9 triplets 1/16T 1/8T 1/4T 1/2T), feedback 0..0.95, tone = low-pass
  // inside the feedback loop in Hz, mix 0..1 (0 = bypass).
  DubDelayDivision = 45,
  DubDelayFeedback = 46,
  DubDelayTone = 47,
  DubDelayMix = 48,
  // EQ FX (dsp/eq.h): four serial biquad bands, 7 params each at 49 + 7*band + field. Per band:
  // On 0/1 (default 0), Type (EqBandType: 0 bell, 1 low shelf, 2 high shelf, 3 low-pass, 4 high-pass,
  // 5 notch), Freq Hz 20..20000, Gain dB -18..18 (bell/shelves), Q 0.1..18 (bell/notch), ShelfSlope
  // 0.1..1 (RBJ S, shelves), Slope 12|24 dB/oct (low-pass/high-pass). All bands off = bypass.
  EqBand1On = 49,
  EqBand1Type = 50,
  EqBand1Freq = 51,
  EqBand1Gain = 52,
  EqBand1Q = 53,
  EqBand1ShelfSlope = 54,
  EqBand1Slope = 55,
  EqBand2On = 56,
  EqBand2Type = 57,
  EqBand2Freq = 58,
  EqBand2Gain = 59,
  EqBand2Q = 60,
  EqBand2ShelfSlope = 61,
  EqBand2Slope = 62,
  EqBand3On = 63,
  EqBand3Type = 64,
  EqBand3Freq = 65,
  EqBand3Gain = 66,
  EqBand3Q = 67,
  EqBand3ShelfSlope = 68,
  EqBand3Slope = 69,
  EqBand4On = 70,
  EqBand4Type = 71,
  EqBand4Freq = 72,
  EqBand4Gain = 73,
  EqBand4Q = 74,
  EqBand4ShelfSlope = 75,
  EqBand4Slope = 76,
  // Glitch FX (dsp/glitch.h): a ring-buffer performance effect. Mix 0..1 (default 0; 0 = bypass, the 0 -> >0 edge engages),
  // Mode (GlitchMode: 0 repeat, 1 tape, 2 gate), Division 0..5 (1/2, 1/4, 1/8, 1/16, 1/32, 1/64 of the transport beat grid),
  // Speed -1..2 (read rate of the loop / the tape; 1 untouched, 0 stopped, negative backwards).
  GlitchMix = 77,
  GlitchMode = 78,
  GlitchDivision = 79,
  GlitchSpeed = 80,
  // Compressor sidechain (dsp/compressor.h): the track bus whose signal drives the detector, 1..kMaxTrackBuses; 0 = off
  // (the compressor listens to its own input, the default). The key is that bus's output after its chain, gain and pan.
  // An unknown, self or cyclic key leaves the compressor on its own input. Not modulatable.
  CompSidechain = 81,
  // Dub Space FX (dsp/dub_space.h): a tape dub echo and a deep synthetic space that can sit inside the echo's feedback loop.
  // EchoOn / SpaceOn 0|1 (default 1; both off = bypass). Time = DubDelayDivision index 0..9 (default 2). Feedback 0..0.92.
  // Tone Hz 200..16000 (the loop low-pass). Size / Depth / Damping / Character 0..1. Mix 0..1 (default 0; 0 = bypass).
  DubSpaceEchoOn = 82,
  DubSpaceSpaceOn = 83,
  DubSpaceTime = 84,
  DubSpaceFeedback = 85,
  DubSpaceTone = 86,
  DubSpaceSize = 87,
  DubSpaceDepth = 88,
  DubSpaceDamping = 89,
  DubSpaceCharacter = 90,
  DubSpaceMix = 91,
  // Decay 0..1: the space's T60, exponential 0.8 s .. ~15 min (default 0.35, about 9 s). Appended after Mix so ids never move.
  DubSpaceDecay = 92,
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
  Pulse = 6,
  Pad = 7,
  Fm = 8,
  Hypersaw = 9,
  PwmBeast = 10,
  // Digital Machines (instruments/dm_*.h): one kind per Machine, one shared FM kernel.
  DmThump = 11,
  DmMetal = 12,
  DmTone = 13,
  DmCrack = 14,
  DmChaos = 15,
  PercSynth = 16,
  Edge = 17,
  Matrix = 18,
  Noise = 19,
  Wobble = 20,
  Electric = 21,
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
  Body = 1,       // 0..1 — round (sine + sub) -> saw (0.5) -> square (1)
  Bite = 2,       // 0..1 — drive into the filter (grit/growl) and accent sensitivity
  Cutoff = 3,     // Hz   — resting cutoff of the 18 dB ladder low-pass (keytracked)
  Resonance = 4,  // 0..1 — filter resonance (squelch)
  Attack = 5,     // s
  Decay = 6,      // s
  Sustain = 7,    // 0..1 — held level
  Release = 8,    // s
  Legato = 9,     // 0/1  — a note on a held note continues the voice (no re-attack)
  Slide = 10,     // s    — legato pitch glide time (0 = immediate)
  EnvMod = 11,    // 0..1 — how far each note's filter envelope opens the cutoff (0..5 octaves)
};

// Pulse (instruments/pulse.h).
enum class PulseParam : int32_t {
  Tune = 0,       // Hz   — root; note pitch is semitones on top
  Cutoff = 1,     // Hz   — resting low-pass cutoff (keytracked)
  Resonance = 2,  // 0..1 — filter resonance (level-compensated)
  Pump = 3,       // 0..1 — rhythmic ducking depth (and recovery length)
  Repeat = 4,     // 0..1 — rhythmic filter opening (and drive) per cycle
  Rate = 5,       // step — 0..5 = 1/1, 1/2, 1/4, 1/8, 1/16, 1/32 of a 4/4 bar
  Diffuse = 6,    // 0..1 — unison spread
  Attack = 7,     // s
  Release = 8,    // s
  PumpRate = 9,   // step — pumper cycle, same steps as Rate; < 0 (default -1) follows Rate
  PumpAttack = 10,  // 0..1 — pump recovery shape: 0 sharp duck, 1 long swell (not a synth attack)
  Pluck = 11,     // 0..1 — per-note pluck: filter snap + pitch click + amp decay to a body
  Grit = 12,      // 0..1 — pre-filter drive: 0 clean, 0.5 (default) classic, 1 gritty
};

// Pad (instruments/pad.h).
enum class PadParam : int32_t {
  Tune = 0,      // Hz   — root; note pitch is semitones on top
  Tone = 1,      // 0..1 — brightness (resting low-pass cutoff, keytracked)
  Movement = 2,  // 0..1 — how far the slow internal drift moves detune, stereo image and filter
  Detune = 3,    // 0..1 — ensemble thickness (detune spread of the saw stack)
  Space = 4,     // 0..1 — stereo width and diffusion
  Attack = 5,    // s
  Release = 6,   // s
};

// Fm (instruments/fm.h).
enum class FmParam : int32_t {
  Tune = 0,     // Hz   — root; note pitch is semitones on top
  Ratio = 1,    // step — 0..7 = modulator ratio 0.5, 1, 2, 3, 4, 5, 1.414, 3.5 (x carrier)
  Timbre = 2,   // 0..1 — resting FM depth (sideband richness)
  Snap = 3,     // 0..1 — extra FM depth at note start that decays (bright attack)
  Tone = 4,     // 0..1 — static low-pass, relative to the note (0 ~ fundamental only)
  Attack = 5,   // s
  Decay = 6,    // s
  Sustain = 7,  // 0..1 — held level
  Release = 8,  // s
  Legato = 9,   // 0/1  — a note on a held note continues the voice (no re-attack)
  Slide = 10,   // s    — legato pitch glide time (0 = immediate)
};

// Hypersaw (instruments/hypersaw.h).
enum class HypersawParam : int32_t {
  Tune = 0,     // Hz   — root; note pitch is semitones on top
  Detune = 1,   // ct   — detune of the outermost saw pair (the other pairs scale from it)
  Spread = 2,   // 0..1 — stereo width (0 = centred, 1 = full distribution)
  Density = 3,  // 0..1 — centre-vs-sides mix: lone centre saw -> full stack
  Cutoff = 4,   // Hz   — low-pass cutoff (keytracked)
  Attack = 5,   // s
  Decay = 6,    // s
  Sustain = 7,  // 0..1 — held level
  Release = 8,  // s
};

// PWM Beast (instruments/pwmbeast.h).
enum class PwmBeastParam : int32_t {
  Tune = 0,       // Hz   — root; note pitch is semitones on top
  Width = 1,      // 0.05..0.95 — static pulse width (0.5 = square; <0.5 and >0.5 are inverted images)
  Pwm = 2,        // 0..1 — PWM depth: how far the width moves around Width
  Rate = 3,       // Hz   — PWM LFO rate (slow sweep up to audio-rate-ish growl)
  Shape = 4,      // 0..1 — PWM source: sine -> skewed ramp (0.5) -> note-envelope sweep (1)
  Harmonics = 5,  // 0..1 — character stage: clean pulse -> rounded, folded, asymmetric
  Drive = 6,      // 0..1 — asymmetric tanh drive after the character stage (gain-compensated)
  Cutoff = 7,     // Hz   — low-pass cutoff (keytracked)
  Resonance = 8,  // 0..1 — low-pass resonance
  EnvAmount = 9,  // 0..1 — how far the internal timbre envelope opens the filter (and drives)
  Sub = 10,       // 0..1 — sub sine (-1 oct) level
  Attack = 11,    // s
  Decay = 12,     // s
  Sustain = 13,   // 0..1 — held level
  Release = 14,   // s
};

// Digital Machines (dsp/fm_kernel.h, instruments/digital_machine.h, instruments/dm_*.h). Every
// Machine puts Tune, Decay and Fm (its "depth" control) at ids 0, 1, 2.
// Thump (instruments/dm_thump.h): kick / tom / tuned percussion / FM bass hit.
enum class DmThumpParam : int32_t {
  Tune = 0,   // Hz   — resting pitch; note pitch is semitones on top
  Decay = 1,  // s    — time to fall ~40 dB
  Fm = 2,     // 0..1 — modulation depth
  Sweep = 3,  // oct  — pitch-envelope depth (starts this far above Tune)
  Punch = 4,  // 0..1 — transient: click level and a fast index burst
  Ratio = 5,  // 0..1 — modulator ratio, magnetic over 0.5 1 1.59 2 2.76 3.5 5.4
  Body = 6,   // 0..1 — membrane overtone level and modulator feedback
  Drive = 7,  // 0..1
};

// Metal (instruments/dm_metal.h): hats / cymbals / clangs / bars and bells.
enum class DmMetalParam : int32_t {
  Tune = 0,    // Hz   — scales the whole cluster
  Decay = 1,   // s
  Fm = 2,      // 0..1 — cross-modulation depth
  Spread = 3,  // 0..1 — ratio set: harmonic -> bar -> 808 -> scattered (magnetic)
  Tone = 4,    // 0..1 — high-pass tilt (0 = full-range bell, 1 = hat)
  Noise = 5,   // 0..1 — noise level and noise phase-modulation
  Chick = 6,   // 0..1 — short bright transient on top of the body
  Drive = 7,   // 0..1
};

// Tone (instruments/dm_tone.h): four-operator pluck / bass / keys / bell / lead / chord stab.
enum class DmToneParam : int32_t {
  Tune = 0,       // Hz   — root; note pitch is semitones on top
  Decay = 1,      // s
  Fm = 2,         // 0..1 — modulation depth
  Structure = 3,  // step — 0 Stack, 1 Twin, 2 Fan, 3 Major, 4 Minor, 5 Sus (chord stabs)
  Ratio = 4,      // 0..1 — modulator ratio, magnetic over 0.5 1 1.41 2 2.76 3 3.5 4 5 7
  Damp = 5,       // 0..1 — how long the modulators outlive the attack (0 = pluck, 1 = as long as the note)
  Attack = 6,     // s
  Hold = 7,       // 0..1 — held level (0 = pluck, 1 = sustained until noteOff)
  Drive = 8,      // 0..1
};

// Crack (instruments/dm_crack.h): snare / clap / rim / noisy percussion.
enum class DmCrackParam : int32_t {
  Tune = 0,   // Hz   — body pitch
  Decay = 1,  // s
  Fm = 2,     // 0..1 — modulation depth, including how hard noise phase-modulates the body
  Body = 3,   // 0..1 — FM body level
  Noise = 4,  // 0..1 — noise level
  Burst = 5,  // 0..1 — 1 burst (snare) -> 5 bursts (clap); also fades the body
  Tone = 6,   // 0..1 — noise band-pass centre
  Drive = 7,  // 0..1
};

// Chaos (instruments/dm_chaos.h): unstable feedback-ring FM, strange digital sounds, metallic leads.
enum class DmChaosParam : int32_t {
  Tune = 0,   // Hz   — root; note pitch is semitones on top
  Decay = 1,  // s
  Chaos = 2,  // 0..1 — ring loop gain: stable -> chaotic (always bounded)
  Ratio = 3,  // 0..1 — ring operator ratio, magnetic
  Fold = 4,   // 0..1 — wavefold
  Noise = 5,  // 0..1 — noise injected into the ring
  Sweep = 6,  // oct  — pitch-envelope depth
  Hold = 7,   // 0..1 — held level (0 = one-shot, 1 = sustained until noteOff)
};

// Percussion Synth (instruments/percsynth.h): two oscillators + noise -> filter -> drive -> amp.
enum class PercSynthParam : int32_t {
  Tune = 0,        // Hz   — osc 1 resting pitch; note pitch is semitones on top (osc 2 follows)
  Osc1Wave = 1,    // step — 0 sine/triangle, 1 saw, 2 square
  Osc1Level = 2,   // 0..1
  Osc2Wave = 3,    // step — 0 sine/triangle, 1 saw, 2 square
  Osc2Pitch = 4,   // st   — osc 2 relative to osc 1 (fractions give inharmonic / metallic tones)
  Osc2Level = 5,   // 0..1
  Osc2Fm = 6,      // 0..1 — osc 1 -> osc 2 frequency modulation (exponential, taken before osc 1's level)
  PitchEnv = 7,    // oct  — how far above the resting pitch osc 1 starts
  PitchDecay = 8,  // s    — time for the sweep to fall ~40 dB
  Osc2Env = 9,     // 0..2 — how much of the pitch sweep osc 2 follows (x PitchEnv)
  Noise = 10,      // 0..1 — noise level
  NoiseTone = 11,  // 0..1 — noise colour: dark (0) -> white (0.5) -> bright (1)
  NoiseDecay = 12, // s    — noise envelope, time to fall ~40 dB
  FilterType = 13, // step — 0 low-pass, 1 high-pass
  Cutoff = 14,     // Hz   — filter cutoff (keytracked)
  Resonance = 15,  // 0..1
  FilterEnv = 16,  // -1..1 — filter envelope depth (+-6 octaves; negative closes/sweeps down)
  FilterDecay = 17, // s   — filter envelope, time to fall ~40 dB
  Attack = 18,     // s
  Decay = 19,      // s
  Sustain = 20,    // 0..1 — held level (0 = a pure one-shot)
  Release = 21,    // s
  Drive = 22,      // 0..1 — character stage: clean -> saturated -> aggressive
};

// Edge (instruments/edge.h): clean, sharp, transient-focused plucks, stabs and leads. A band-limited
// harmonic-limited oscillator, a signed pitch transient, and two filter stages with routing.
enum class EdgeParam : int32_t {
  Tune = 0,        // Hz   — root; note pitch is semitones on top
  Shape = 1,       // 0..1 — saw (0) -> square (0.5) -> narrow pulse (1)
  Bright = 2,      // 0..1 — oscillator brightness: harmonic count 1 (sine) .. 256 (bright saw)
  BrightEnv = 3,   // -1..1 — tone envelope depth on brightness (positive: bright onset that settles)
  Detune = 4,      // cents — second oscillator; fades in over the first 10 cents (0 = single)
  PitchEnv = 5,    // st   — pitch transient: start offset, signed (positive = from above)
  PitchDecay = 6,  // s    — time for the transient to fall ~40 dB
  Routing = 7,     // step — 0 filter A only, 1 series (A -> B), 2 parallel (A + B)
  FilterA = 8,     // step — 0 LP 12 dB, 1 LP 24 dB, 2 HP 12 dB, 3 band-pass
  FilterB = 9,     // step — same modes as A
  Cutoff = 10,     // Hz   — filter A cutoff (keytracked)
  Spread = 11,     // oct  — filter B cutoff relative to A (signed)
  Resonance = 12,  // 0..1 — shared; in series B takes a fraction of A's
  FilterEnv = 13,  // -1..1 — tone envelope depth on both cutoffs (+-6 octaves)
  ToneDecay = 14,  // s    — tone envelope (filter + brightness), time to fall ~40 dB
  LfoRate = 15,    // Hz
  LfoPitch = 16,   // 0..1 — LFO -> pitch (vibrato), up to +-1.5 semitones
  LfoFilter = 17,  // 0..1 — LFO -> both cutoffs, up to +-3 octaves
  Attack = 18,     // s
  Decay = 19,      // s
  Sustain = 20,    // 0..1 — held level (0 = a pure one-shot)
  Release = 21,    // s
};

// Noise (instruments/noise.h): noise as the primary source. Source -> colour (spectral tilt) -> texture
// (grain gate) -> six-band resonator bank (+ dry blend) -> drive -> tone -> envelope. Layout for a compact UI:
// SOURCE (Source, Color, Texture) / RESONANCE (Tune, Resonance, Spread, Sweep) / SHAPE (Attack, Decay, Density)
// / CHARACTER (Drive, Tone, Mix); Sustain and Release are the long-form extras.
enum class NoiseParam : int32_t {
  Source = 0,     // step — 0 white, 1 pink, 2 brown, 3 metal, 4 crackle, 5 burst
  Color = 1,      // 0..1 — spectral tilt: dark rumble (0) -> balanced (0.5) -> bright / hat territory (1)
  Texture = 2,    // 0..1 — smooth (0) -> grainy -> granular -> crackling / sparse (1)
  Density = 3,    // Hz   — events per second: grains (Texture), pops (crackle), bursts (burst)
  Tune = 4,       // Hz   — resonator root; note pitch is semitones on top
  Resonance = 5,  // 0..1 — ring: broad band (0) -> pitched -> long bell-like decay (1)
  Spread = 6,     // 0..1 — band layout: unison (0) -> harmonic -> bell (inharmonic) -> metallic cluster (1)
  Sweep = 7,      // oct  — resonators move with the amplitude envelope (+ rises with it, - falls)
  Attack = 8,     // s
  Decay = 9,      // s
  Sustain = 10,   // 0..1 — held level (0 = a pure one-shot)
  Release = 11,   // s
  Drive = 12,     // 0..1 — clean -> saturated -> ringing feedback
  Tone = 13,      // 0..1 — output tilt: dark (0) -> flat (0.5) -> bright (1)
  Mix = 14,       // 0..1 — raw noise (0) -> resonators (1), equal power
};

// Wobble (instruments/wobble.h): an opinionated bass-music bass. Eight front-panel macros (Wobble, Rate, Growl,
// Bite, Weight, Drive, Cutoff, Resonance) plus Shape; Tune, Release, Glide and FreeRate are the long-form extras.
// Wobble is one integrated modulation (cutoff, growl, resonance and the vowel move together), not a bare filter LFO.
enum class WobbleParam : int32_t {
  Wobble = 0,     // 0..1 — depth of the coordinated movement: steady (0) -> obvious wub -> violent (1)
  Rate = 1,       // step — tempo-synced cycle length, 0 = 1/1 ... 11 = 1/32 (dotted and triplet steps between)
  Shape = 2,      // step — 0 smooth, 1 square-ish, 2 ramp, 3 random (stepped)
  Growl = 3,      // 0..1 — harmonic aggression: clean heavy bass (0) -> FM + formant growl (1)
  Bite = 4,       // 0..1 — upper-mid cut: attack zap, presence band, abrasive edge
  Weight = 5,     // 0..1 — low-end body: lean (0) -> huge (1); the sub path never wobbles
  Drive = 6,      // 0..1 — gain-compensated saturation: clean (0) -> dense (1)
  Cutoff = 7,     // Hz   — centre of the wobble sweep (the open point sits above it)
  Resonance = 8,  // 0..1 — smooth (0) -> squelchy (0.5) -> screaming, short of self-oscillation (1)
  Tune = 9,       // Hz   — root; note pitch is semitones on top
  Release = 10,   // s
  FreeRate = 11,  // Hz   — 0 = tempo-synced via Rate; > 0 = free-running wobble at this rate
  Glide = 12,     // s    — slide time when a note is tied to the held one
};

// Electric (instruments/electric.h): a digital electric-keyboard voice. Eight front-panel macros (Body, Strike, Bell,
// Warmth, Age, Crush, Cutoff, Resonance) plus Decay; Tune, Release and Width are the long-form extras. Macros, not
// operators: the FM/PM, the envelopes and the gain staging are fixed inside the instrument.
enum class ElectricParam : int32_t {
  Body = 0,       // 0..1 — core of the key: light and thin (0) -> full, strong fundamental plus a touch of sub (1)
  Strike = 1,     // 0..1 — attack: soft and rounded (0) -> struck, percussive, bright transient (1)
  Bell = 2,       // 0..1 — inharmonic upper component: pure (0) -> bell, metallic, glassy (1)
  Warmth = 3,     // 0..1 — clean and digital (0) -> harmonically dense and organic (1); level-compensated
  Age = 4,        // 0..1 — precise (0) -> slightly imperfect: pitch drift, per-note variation, slow tonal movement
  Crush = 5,      // 0..1 — clean (0) -> sample-rate and bit-depth reduction inside the voice (1)
  Cutoff = 6,     // Hz   — low-pass cutoff (key-tracked a little; the strike opens it briefly)
  Resonance = 7,  // 0..1 — filter emphasis, bounded short of self-oscillation
  Decay = 8,      // 0..1 — short, percussive (0) -> long, sustained (1)
  Tune = 9,       // Hz   — root; note pitch is semitones on top
  Release = 10,   // s    — key-up time (scaled by Decay: x(0.5 + decay))
  Width = 11,     // 0..1 — stereo spread of the oscillator pair and the bell; 0 is mono
};

// Matrix (instruments/matrix.h): a semi-modular subtractive voice. A fixed set of modules (VCO, VCF,
// VCA, two LFOs, two envelopes, noise) with patch points: eight outs and eleven ins, joined by a matrix
// of 88 cells, one per (out, in) pair, addressed by matrixAmountParam(). Each cell is a signed amount,
// -1..1, 0 = no cable. The usual voice is simply the default patch (VCO > VCF in, VCF > VCA in, VCA > out,
// ENV 1 > VCA level, ENV 2 > cutoff), so every connection, the audio path included, can be removed or
// redirected. Controls 0..20 are the modules' own knobs; ids 21..108 are the matrix.
enum class MatrixParam : int32_t {
  Tune = 0,         // Hz   — root; note pitch is semitones on top
  Wave = 1,         // step — 0 saw, 1 pulse, 2 triangle
  Pwm = 2,          // 0.05..0.95 — pulse width before patches (pulse wave only)
  Cutoff = 3,       // Hz   — filter cutoff before keytrack and patches
  Resonance = 4,    // 0..1
  Drive = 5,        // 0..1 — saturation ahead of the filter
  Mode = 6,         // step — 0 LP 24 dB (ladder), 1 LP 12 dB, 2 band-pass, 3 HP 12 dB
  Keytrack = 7,     // 0..1 — how far the cutoff follows note pitch (1 = fully)
  Level = 8,        // 0..1 — VCA level before patches (0 = the amp opens only by patch)
  Lfo1Rate = 9,     // Hz
  Lfo1Wave = 10,    // step — 0 sine, 1 triangle, 2 saw, 3 square, 4 sample & hold
  Lfo2Rate = 11,    // Hz
  Lfo2Wave = 12,    // step — as LFO 1
  Env1Attack = 13,  // s    — ENV 1 also sets how long a note lives
  Env1Decay = 14,   // s
  Env1Sustain = 15, // 0..1
  Env1Release = 16, // s
  Env2Attack = 17,  // s
  Env2Decay = 18,   // s
  Env2Sustain = 19, // 0..1
  Env2Release = 20, // s
  AmountBase = 21,  // first matrix cell; see matrixAmountParam()
};

// The patch points. An out is a signal a module produces; an in is a place a signal can be sent, and
// everything patched into an in is summed. The indices are part of the contract. The order of the outs is
// also the order the voice computes them in each sample: an in hears an out of an earlier module in the
// same sample and an out of a later one (or of its own module) one sample late, which is what makes a
// loop such as VCF out > cutoff well defined.
enum class MatrixOut : int32_t { Vco = 0, Noise = 1, Lfo1 = 2, Lfo2 = 3, Env1 = 4, Env2 = 5, Vcf = 6, Vca = 7 };
enum class MatrixIn : int32_t {
  Pitch = 0, Pwm = 1, VcfIn = 2, Cutoff = 3, Resonance = 4, Drive = 5, VcaIn = 6, Level = 7,
  Lfo1Rate = 8, Lfo2Rate = 9, Out = 10,
};
inline constexpr int32_t kMatrixOutCount = 8;
inline constexpr int32_t kMatrixInCount = 11;
inline constexpr int32_t kMatrixAmountCount = kMatrixOutCount * kMatrixInCount;
inline constexpr int32_t matrixAmountParam(MatrixOut o, MatrixIn i) {
  return static_cast<int32_t>(MatrixParam::AmountBase) + static_cast<int32_t>(o) * kMatrixInCount +
         static_cast<int32_t>(i);
}

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
