// Public types for the audio runtime. Nothing in this file knows about pads, patterns,
// trackers, or any other application concept — see ARCHITECTURE.md for the boundary this
// file enforces.

/** Opaque identifier for a sample resource held by the runtime. */
export type SampleId = number;

/** Opaque handle for a single triggered voice. Not guaranteed to stay valid forever —
 * voices can be stolen or finish playback; operations on a dead handle are no-ops. */
export type VoiceHandle = number;

/** Opaque identifier for a mixer bus (e.g. the default "master" bus). */
export type BusId = number;

export const MASTER_BUS: BusId = 0;

/** How many buses beyond MASTER_BUS an application can allocate via
 * `AudioRuntime.createBus()` — e.g. one per sequencer track, each with its own filter+delay
 * chain (see NodeParam) that sums into MASTER_BUS before MASTER_BUS's own chain runs.
 * Mirrors native/src/engine.h's `kMaxTrackBuses` — keep in sync (see that file's comment for
 * why a mismatch is safe either way, just wasteful or overly restrictive). 64 covers 4 banks of
 * 16 pads (ECS-84). */
export const MAX_TRACK_BUSES = 64;

/** Metadata the runtime exposes for a loaded sample. Mirrors native/src/sample_store.h. */
export interface SampleMetadata {
  id: SampleId;
  name: string;
  channels: number;
  /** Native sample rate of the source audio, preserved even though playback may resample. */
  sampleRate: number;
  /** Length in frames at the sample's native sample rate. */
  length: number;
  /** Duration in seconds (length / sampleRate). */
  duration: number;
  /** Approximate resident memory in bytes (planar float32 PCM). */
  byteLength: number;
  /** Cue regions last set via loadSample({ regions }) / setSampleRegions(); absent if none. */
  regions?: SampleRegion[];
}

/** A [start, end) span of a sample's frames. Describes the audio only — it is not a playback
 * mode. A trigger with `region: i` plays region `i` (chop playback); a trigger without it plays
 * the whole sample / start..end as before. No audio is copied. */
export interface SampleRegion {
  start: number;
  /** Exclusive. */
  end: number;
}

/** Chop-mode note mapping: note `n` plays region `n - rootNote` (consecutive notes = consecutive
 * regions, in whatever note unit the caller uses, e.g. MIDI). Returns undefined when the note
 * falls outside `regionCount`, so the caller can skip it. */
export function regionForNote(note: number, rootNote: number, regionCount: number): number | undefined {
  const index = Math.round(note) - rootNote;
  return index >= 0 && index < regionCount ? index : undefined;
}

/** Runtime/hardware capabilities and current policy limits. Not necessarily fixed hardware
 * facts — maxVoices in particular is a runtime policy that can adapt to the device. */
export interface RuntimeCapabilities {
  sampleRate: number;
  outputChannels: number;
  maxVoices: number;
  /** AudioWorklet render quantum size in frames (128 on all current browsers). */
  renderQuantumFrames: number;
  /** Longest capture window (seconds) armCapture()/startCapture() will actually record — see
   * native/src/capture.h's kMaxCaptureSeconds. A caller should check a requested capture's
   * duration against this *before* calling armCapture(), since that call throws rather than
   * silently truncating a too-long request. */
  maxCaptureSeconds: number;
  /** Synthesized instrument kinds the engine provides, with their self-described parameters,
   * as reported by the native engine itself (single source of truth). See
   * AudioRuntime.createInstrument(). */
  instruments: InstrumentKindInfo[];
  /** LFO modulation limits and option lists (see AudioRuntime.setInstrumentLfo()). */
  modulation: ModulationCapabilities;
}

/** Point-in-time diagnostics for observability / stress testing. */
export interface RuntimeDiagnostics {
  activeVoices: number;
  loadedSamples: number;
  sampleMemoryBytes: number;
}

/** Real-time-safe voice playback parameters. All optional; omitted fields keep the
 * sample's natural defaults (full length, unity gain/rate, no loop). */
export interface TriggerParams {
  sampleId: SampleId;
  /** Linear gain multiplier, not dB. Default 1.0. */
  gain?: number;
  /** Playback rate multiplier; 2.0 is +1 octave, 0.5 is -1 octave. Default 1.0. */
  rate?: number;
  /** Convenience alternative to `rate`, in semitones. Combined multiplicatively with rate. */
  pitch?: number;
  /** Start offset in source frames. Default 0. */
  start?: number;
  /** End offset in source frames (exclusive). Default: sample length. */
  end?: number;
  /** Chop playback: play this cue region (0-based index into the sample's regions, see
   * setSampleRegions()) instead of start/end. Rate/pitch still apply. A region that doesn't
   * exist plays nothing. Omit for normal whole-sample playback. */
  region?: number;
  /** Loop between start/end once playback reaches `end`. Default false. */
  loop?: boolean;
  /** Play the [start, end) region backwards. Default false. */
  reverse?: boolean;
  /** Bus this voice's output is summed into. Default MASTER_BUS. */
  bus?: BusId;
  /** Absolute engine time (seconds, same domain as getCurrentTime()) to start at.
   * Omit for "as soon as possible" (next render quantum). */
  time?: number;
  /** Note duration in seconds. When set, the engine auto-releases the voice (the same
   * short envelope taper as an explicit release() call) this many seconds after playback
   * actually starts — computed and applied entirely on the audio render thread, not via a
   * second timed message from the host. Omit for indefinite/natural-length playback (the
   * voice plays until it explicitly release()s/stop()s or the sample itself ends). This is
   * what lets a sequencer express note length as data, not as a second scheduled call. */
  duration?: number;
  /** Stereo position, -1 (full left) .. 0 (centre, default) .. +1 (full right); clamped. Mono
   * samples are placed with a constant-power law, stereo samples are balanced. Centre leaves
   * the signal exactly as it was before pan existed. See docs/panning.md. */
  pan?: number;
}

/** A single scheduled trigger, as produced by an external sequencer. Deliberately generic:
 * no padId, no step index, no track index. See src/sequencing for an example compiler that
 * turns an application-specific pattern into a list of these. */
export interface ScheduledEvent extends TriggerParams {
  time: number;
}

// Numeric IDs are a manually-maintained contract with native/src/params.h — kept as a
// tiny, explicit enum rather than a generated binding because the parameter set is small
// and changes rarely; test/paramIds.test.ts pins these literal values so an accidental
// reorder is caught even though the two files can't be checked against each other directly.
//
// Deliberately a regular `enum`, not `const enum`: tsup's declaration bundling emits a
// package's `const enum` as an *ambient* `declare const enum` in the shipped .d.ts, which
// TypeScript refuses to inline member access for under `isolatedModules` (TS2748) — a mode
// Vite/esbuild-based consumers require. A regular enum compiles to a real runtime object,
// so external packages (see "Using this as a package") can actually write
// `VoiceParam.Gain` without that consumer needing to disable isolatedModules. Verified by
// building this tracker app against the published package.
export enum VoiceParam {
  Gain = 0,
  Rate = 1,
  FilterCutoff = 2,
  FilterResonance = 3,
  /** 0 = LowPass, 1 = HighPass (see FilterMode). */
  FilterMode = 4,
  /** Filter slope in dB/octave: 12 (default) or 24 (see FilterSlope). */
  FilterSlope = 5,
  /** Stereo position, -1 (full left) .. 0 (centre, default) .. +1 (full right). Constant-power
   * for mono samples, balance for stereo ones; smoothed (~5 ms). See docs/panning.md. */
  Pan = 6,
}

// Sample-addressed parameters — a persistent property of the sample itself (set via
// setSampleParameter(), or loadSample()'s `gain` option at load time), not of any one playing
// voice. This is the signal-staging primitive furthest upstream: an application sets it once
// per instrument/sample (e.g. to level-match a quiet kick against a hot snare) and leaves it
// alone, as opposed to TriggerParams.gain/VoiceParam.Gain, which a caller is expected to drive
// per trigger (velocity, a future per-cell level — see ARCHITECTURE.md's "How samples are
// represented" for the full signal-level hierarchy this establishes). Multiplies with
// VoiceParam.Gain rather than replacing it — both apply, upstream of a voice's own filter and
// of the bus/track chain it renders into. Separate id space from VoiceParam/NodeParam, same
// reason NodeParam's own doc comment gives for being separate from VoiceParam: a SampleParam
// and a VoiceParam that happen to share a numeric value don't mean the same thing.
//
// EnvAttack..EnvRelease are the low-level ids behind the amplitude envelope. Applications
// should use `EnvelopeParams` with `AudioRuntime.setSampleEnvelope()` / `loadSample({ envelope })`
// instead, which speaks seconds and a 0..1 level and hides these ids.
export enum SampleParam {
  Gain = 0,
  /** Envelope attack, seconds (0..30). */
  EnvAttack = 1,
  /** Envelope decay, seconds (0..30). */
  EnvDecay = 2,
  /** Envelope sustain level (0..1, linear gain fraction of peak). */
  EnvSustain = 3,
  /** Envelope release, seconds (0..30). */
  EnvRelease = 4,
  /** Instrument default pan, -1..+1 (default 0). Added to each voice's own pan and clamped;
   * applies live to playing voices. See docs/panning.md. */
  Pan = 5,
}

/** Amplitude ADSR envelope applied to every voice of an instrument/sample — the public,
 * implementation-independent control model. Times are in seconds; `sustain` is a 0..1 level
 * relative to the peak. All fields are optional; omitted fields keep their current value
 * (defaults: attack 0, decay 0, sustain 1, release 0.01 — i.e. no audible envelope beyond a
 * 10 ms click-free release). Out-of-range values are clamped (times 0..30 s, sustain 0..1).
 *
 * Behaviour: note-on starts attack (0 -> 1), then decay (1 -> sustain), then holds sustain
 * until release() / the trigger's `duration` elapses, then release (level -> 0). The voice
 * stays alive for the whole release and is reported ended only once it reaches silence.
 * Segments are linear in amplitude. Retriggering a still-sounding voice (voice steal, or
 * note-on during release) continues the attack from the current level rather than snapping.
 * A time of 0 is an instant stage. A stage of N frames renders N frames short of its target
 * and reaches it on frame N; release renders N non-zero frames, then silence.
 *
 * Live edits (deterministic): attack, decay and sustain are captured when a note starts, so
 * editing them affects only notes triggered afterward. Release is read at the moment of
 * note-off, so a changed release applies to notes that are still held.
 */
export interface EnvelopeParams {
  /** Seconds to rise from silence to peak. */
  attack?: number;
  /** Seconds to fall from peak to the sustain level. */
  decay?: number;
  /** Held level, 0..1. */
  sustain?: number;
  /** Seconds to fall from the level at note-off to silence. */
  release?: number;
}

// Bus-addressed parameters — identical shape on MASTER_BUS and on any bus returned by
// createBus(). FilterCutoff/FilterResonance/FilterMode drive a BiquadFilter — the same
// DSPNode class Voice's per-voice filter uses, composed once more on the bus instead of
// per-voice (see native/src/bus.h) — giving a public, generic "bus processing node" surface
// a future effect (delay is already here; reverb/compressor/EQ later) can sit alongside
// without any change to this class's shape.
export enum NodeParam {
  DelayTime = 0,
  DelayFeedback = 1,
  DelayMix = 2,
  BusGain = 3,
  FilterCutoff = 4,
  FilterResonance = 5,
  /** 0 = LowPass, 1 = HighPass (see FilterMode). */
  FilterMode = 6,
  /** Tank per-loop feedback gain (0..1) — controls reverb tail length. Clamped native-side
   * to [0, 0.9999] to guarantee a decaying (not runaway) tank. */
  ReverbDecay = 7,
  /** Tank high-frequency damping (0..1) — 0 is bright/undamped, 1 is heavily damped/dark. */
  ReverbDamping = 8,
  /** Dry/wet mix (0..1). 0 = bypass, matching Delay/Filter's "inert until parameterized"
   * convention — a freshly-configured bus's reverb costs one cheap early-out per block. */
  ReverbMix = 9,
  /** Compressor threshold, dB (-60..0). Above this instantaneous peak level, gain reduction
   * starts to apply (see CompKnee for the transition width). */
  CompThreshold = 10,
  /** Compressor ratio (1..20). 1 = no compression (the default, and the node's bypass
   * convention — see CompressorNode's doc comment: any setParam call still un-bypasses it,
   * matching BiquadFilter, but a ratio of 1 is mathematically a no-op regardless). */
  CompRatio = 11,
  /** Attack time, seconds (0.0001..1) — how fast gain reduction engages once the threshold is
   * exceeded. */
  CompAttack = 12,
  /** Release time, seconds (0.001..3) — how fast gain reduction recovers once the level drops
   * back below threshold. */
  CompRelease = 13,
  /** Soft-knee width, dB (0..24) — how gradually compression ramps in around the threshold,
   * rather than switching on abruptly. */
  CompKnee = 14,
  /** Makeup gain, dB (-24..24), applied after compression to restore perceived loudness. */
  CompMakeup = 15,
  /** Saturation pre-gain, dB (0..40), applied before the waveshaper — how hard the signal
   * drives into the curve. */
  SatDrive = 16,
  /** Saturation asymmetry (-1..1, 0 = symmetric) — biases the waveshaper's curve to produce
   * even-harmonic ("tube-like") content; a DC blocker downstream pulls the resulting bias
   * back toward the signal's original average level rather than leaving it fully offset. */
  SatAsymmetry = 17,
  /** Saturation output gain, dB (-24..24), applied after the waveshaper to trim/restore
   * level. */
  SatOutputGain = 18,
  /** Dry/wet mix (0..1). 0 = bypass, matching Delay/Reverb/Filter's "inert until
   * parameterized" convention. */
  SatMix = 19,
  /** Chorus/flanger LFO speed, Hz (0.01..10). Chorus and flanger are the same primitive at
   * different `ChorusFlangerDelay`/`ChorusFlangerFeedback` settings, not different node
   * types — see `docs/chorus-flanger-node.md`. */
  ChorusFlangerRate = 20,
  /** Chorus/flanger LFO peak excursion, ms (0..20), added to/subtracted from the center
   * delay (`ChorusFlangerDelay`). */
  ChorusFlangerDepth = 21,
  /** Chorus/flanger center/base delay, ms (0.1..40). A longer delay (~15-30ms) with little
   * feedback reads as chorus; a short delay (~1-10ms) with feedback dialed up reads as
   * flanger. */
  ChorusFlangerDelay = 22,
  /** Chorus/flanger comb feedback (-0.95..0.95). 0 for a plain chorus; nonzero (either sign)
   * for a flanger's resonant comb sweep. */
  ChorusFlangerFeedback = 23,
  /** Fraction of one LFO cycle (0..1) channel 1's modulation phase leads channel 0's by —
   * widens the effect in stereo. 0 = both channels modulate in lockstep (mono-compatible but
   * narrow); ~0.25 is a typical wide-chorus default. */
  ChorusFlangerStereoPhase = 24,
  /** Dry/wet mix (0..1). 0 = bypass, matching Delay/Reverb/Saturation/Filter's "inert until
   * parameterized" convention. */
  ChorusFlangerMix = 25,
  /** Filter slope in dB/octave: 12 (default) or 24 (see FilterSlope). */
  FilterSlope = 26,
  /** Bus output balance, -1 (full left) .. 0 (centre, default) .. +1 (full right), applied
   * after BusGain; smoothed (~5 ms). See docs/panning.md. */
  BusPan = 27,
  /** Phaser LFO speed, Hz (0.01..10). */
  PhaserRate = 28,
  /** Phaser sweep depth (0..1); 1 sweeps +-2 octaves around PhaserCenter. */
  PhaserDepth = 29,
  /** Phaser centre frequency, Hz (100..5000). */
  PhaserCenter = 30,
  /** Phaser feedback (-0.95..0.95). */
  PhaserFeedback = 31,
  /** Number of all-pass stages, rounded to even (2..12). 4 = two notches. */
  PhaserStages = 32,
  /** Dry/wet (0..1). 0 = bypass. */
  PhaserMix = 33,
  /** Bitcrusher bit depth (1..16). */
  CrushBits = 34,
  /** Bitcrusher sample-hold factor in frames (1..64); 1 = no decimation. */
  CrushDownsample = 35,
  /** Dry/wet (0..1). 0 = bypass. */
  CrushMix = 36,
}

/** Opaque identifier of one effect instance in a bus's FX chain, returned by
 * `AudioRuntime.addEffect()`. Unique per runtime, so `filter` (an EffectKind) and the two
 * filters you added (two EffectIds) are different things. */
export type EffectId = number;

/** The kind of DSP an effect instance runs. Mirrors native/src/fx_chain.h `FxKind` (pinned by
 * test/paramIds.test.ts). Any number of instances of each kind may be in one chain. A kind's
 * parameters are the NodeParam ids with its prefix (FilterCutoff.., DelayTime.., ...). */
export enum EffectKind {
  Filter = 0,
  ChorusFlanger = 1,
  Delay = 2,
  Reverb = 3,
  Compressor = 4,
  Saturation = 5,
  Phaser = 6,
  Bitcrusher = 7,
}

/** One entry of a bus's FX chain, as returned by `AudioRuntime.getEffectChain()`. */
export interface EffectInfo {
  id: EffectId;
  kind: EffectKind;
  enabled: boolean;
}

/** Max effect instances per bus chain. Mirrors native `kMaxFxPerBus`. */
export const MAX_EFFECTS_PER_BUS = 16;

/** Shared value space for VoiceParam.FilterMode / NodeParam.FilterMode — not itself a
 * param id, just what the 0/1 float value passed to setVoiceParameter/setNodeParameter
 * means for either filter instance. */
export enum FilterMode {
  LowPass = 0,
  HighPass = 1,
}

/** Value space for VoiceParam.FilterSlope / NodeParam.FilterSlope: the rolloff in dB/octave.
 * 12 = two-pole (default, the original behavior), 24 = four-pole. */
export enum FilterSlope {
  Db12 = 12,
  Db24 = 24,
}

/** Opaque identifier of one instrument instance, returned by `AudioRuntime.createInstrument()`. */
export type InstrumentId = number;

/** Kinds of synthesized instrument. Mirrors native/src/params.h `InstrumentKind` (pinned by
 * test/paramIds.test.ts). Each kind is a purpose-built machine with its own small parameter
 * set — not a generic synth. */
export enum InstrumentKind {
  Kick = 0,
  Snare = 1,
  Clap = 2,
  Tom = 3,
  HiHat = 4,
  Bass = 5,
  Pulse = 6,
  Pad = 7,
  Fm = 8,
}

/** Kick controls. Mirrors native `KickParam`. Ranges/defaults/units come from
 * `getInstrumentParams(InstrumentKind.Kick)` so UIs need not hardcode them. */
export enum KickParam {
  /** Hz — pitch the body settles on. */
  Tune = 0,
  /** Octaves above Tune the pitch sweep starts. */
  PitchDrop = 1,
  /** Seconds — how fast the sweep falls. */
  PitchDecay = 2,
  /** Seconds — body length. */
  Decay = 3,
  /** 0..1 — attack click level. */
  Punch = 4,
  /** 0..1 — saturation amount. */
  Drive = 5,
}

/** Snare controls. Mirrors native `SnareParam`. */
export enum SnareParam {
  /** Hz — body pitch. */
  Tune = 0,
  /** Seconds — overall length. */
  Decay = 1,
  /** 0..1 — transient + brightness of the noise. */
  Snap = 2,
  /** 0..1 — noise vs tonal body balance. */
  Noise = 3,
}

/** Clap controls. Mirrors native `ClapParam`. */
export enum ClapParam {
  /** Hz — centre of the noise band. */
  Tune = 0,
  /** Seconds — length of the final tail. */
  Decay = 1,
  /** 0..1 — time between the burst cluster's hits. */
  Spread = 2,
  /** 0..1 — sharpness of the bursts. */
  Snap = 3,
}

/** Tom controls. Mirrors native `TomParam`. */
export enum TomParam {
  /** Hz — resting fundamental (note `pitch` offsets it in semitones). */
  Tune = 0,
  /** Octaves above Tune the strike starts. */
  PitchDrop = 1,
  /** Seconds — ring length. */
  Decay = 2,
  /** 0..1 — membrane overtones + stick tick. */
  Body = 3,
  /** 0..1 — damping: shorter ring, darker tone. */
  Damp = 4,
}

/** Hi-hat controls. Mirrors native `HiHatParam`. */
export enum HiHatParam {
  /** Semitones — offset of the metallic cluster. */
  Tune = 0,
  /** Seconds — short = closed, long = open. */
  Decay = 1,
  /** 0..1 — noise -> inharmonic metallic cluster. */
  Metal = 2,
  /** 0..1 — brightness. */
  Tone = 3,
}

/** Bass controls. Mirrors native `BassParam`. Sustained: pass `duration` on notes (or call
 * `instrumentNoteOff`) to end them. */
export enum BassParam {
  /** Hz — root; note `pitch` is semitones on top. */
  Tune = 0,
  /** 0..1 — round/subby (sine + sub) to rich (saw). */
  Body = 1,
  /** 0..1 — onset drive/brightness, and how much velocity changes timbre. */
  Bite = 2,
  /** Hz — resting low-pass cutoff (keytracked). */
  Cutoff = 3,
  /** 0..1 — filter resonance. */
  Resonance = 4,
  /** Seconds. */
  Attack = 5,
  /** Seconds. */
  Decay = 6,
  /** 0..1 — held level. */
  Sustain = 7,
  /** Seconds. */
  Release = 8,
  /** 0/1 (>= 0.5 = on) — a note landing on a still-held note continues the voice: no re-attack,
   * pitch changes (see `Slide`). Needs notes that overlap or abut (duration >= step). */
  Legato = 9,
  /** Seconds — legato pitch glide time (linear in pitch); 0 = immediate. */
  Slide = 10,
}

/** Pulse controls. Mirrors native `PulseParam`. Pump and repeat are locked to the engine's beat
 * grid (see `AudioRuntime.setTempo()`), not to note starts. Sustained: pass `duration` on notes
 * (or call `instrumentNoteOff`) to end them. */
export enum PulseParam {
  /** Hz — root; note `pitch` is semitones on top. */
  Tune = 0,
  /** Hz — resting low-pass cutoff (keytracked). */
  Cutoff = 1,
  /** 0..1 — filter resonance. */
  Resonance = 2,
  /** 0..1 — rhythmic ducking depth (and recovery length). */
  Pump = 3,
  /** 0..1 — rhythmic filter opening (and drive) per cycle. */
  Repeat = 4,
  /** Step 0..5 — 1/1, 1/2, 1/4, 1/8, 1/16, 1/32 of a 4/4 bar. */
  Rate = 5,
  /** 0..1 — unison spread. */
  Diffuse = 6,
  /** Seconds. */
  Attack = 7,
  /** Seconds. */
  Release = 8,
}

/** Pad controls. Mirrors native `PadParam`. Sustained and free-running (not tempo-synced): pass
 * `duration` on notes (or call `instrumentNoteOff`) to end them. A new note on a sounding instance
 * moves the previous note into its release, so successive notes overlap. */
export enum PadParam {
  /** Hz — root; note `pitch` is semitones on top. */
  Tune = 0,
  /** 0..1 — brightness. */
  Tone = 1,
  /** 0..1 — slow drift of detune, stereo image and filter. */
  Movement = 2,
  /** 0..1 — ensemble thickness. */
  Detune = 3,
  /** 0..1 — stereo width and diffusion. */
  Space = 4,
  /** Seconds. */
  Attack = 5,
  /** Seconds. */
  Release = 6,
}

/** FM controls. Mirrors native `FmParam`. Sustained: pass `duration` on notes (or call
 * `instrumentNoteOff`) to end them. `legato` >= 0.5 makes a note on a held note continue the voice. */
export enum FmParam {
  /** Hz — root; note `pitch` is semitones on top. */
  Tune = 0,
  /** Step 0..7 — modulator ratio 0.5, 1, 2, 3, 4, 5, 1.414, 3.5 (relative to the note). */
  Ratio = 1,
  /** 0..1 — resting FM depth (sideband richness). */
  Timbre = 2,
  /** 0..1 — extra FM depth at note start that decays (bright attack). */
  Snap = 3,
  /** 0..1 — static low-pass relative to the note. */
  Tone = 4,
  /** Seconds. */
  Attack = 5,
  /** Seconds. */
  Decay = 6,
  /** 0..1 — held level. */
  Sustain = 7,
  /** Seconds. */
  Release = 8,
  /** 0/1 — legato. */
  Legato = 9,
  /** Seconds — legato glide time. */
  Slide = 10,
}

/** How the engine's LFO layer may modulate a parameter, declared by the synth that owns it:
 * "none" = not a modulation target; "linear" = moves in its own units (100% = half the range
 * either side of the base); "log" = moves in octaves (Hz-valued controls). */
export type ModulationDomain = "none" | "linear" | "log";

/** Self-description of one instrument control. */
export interface InstrumentParamInfo {
  id: number;
  name: string;
  min: number;
  max: number;
  default: number;
  unit: string;
  /** Whether (and in which domain) an LFO can target this control. A modulation target picker
   * lists exactly the params whose value is not "none". */
  modulation: ModulationDomain;
}

/** LFO waveforms. Mirrors native `LfoWaveform`; names come from `ModulationCapabilities`. */
export enum LfoWaveform {
  Sine = 0,
  Triangle = 1,
  Square = 2,
  Saw = 3,
}

/** One LFO routed to one instrument parameter. See docs/lfo-modulation.md. */
export interface InstrumentLfo {
  /** Instrument param id (from the kind's enum) to modulate; must have `modulation !== "none"`. */
  target: number;
  /** -1..+1. 1 = the LFO sweeps half the parameter's range (see ModulationDomain) either side of
   * the base value; negative inverts. 0 = no modulation. */
  amount: number;
  waveform?: LfoWaveform;
  /** false (default): free-running at `rateHz`. true: locked to the beat grid (setTempo) at
   * `division`. */
  sync?: boolean;
  /** Free mode rate, Hz (default 1). */
  rateHz?: number;
  /** Sync mode: index into `ModulationCapabilities.divisions` (default "1/4"). */
  division?: number;
  /** Start phase / phase offset, 0..1 (default 0). */
  phase?: number;
  /** Free mode only: restart the cycle at every note-on (default false = truly free-running). */
  retrigger?: boolean;
}

/** What the LFO layer supports, reported by the engine. */
export interface ModulationCapabilities {
  /** LFO slots per instrument (slot indices 0..maxLfos-1). */
  maxLfos: number;
  /** Waveform display names, indexed by `LfoWaveform`. */
  waveforms: string[];
  /** Sync divisions, slowest first; `beats` = cycle length in quarter-note beats. */
  divisions: { name: string; beats: number }[];
  minRateHz: number;
  maxRateHz: number;
}

export interface InstrumentKindInfo {
  kind: InstrumentKind;
  /** Stable machine identifier ("kick", "hihat"). Persist/refer to this, never `name`. */
  id: string;
  /** Human-facing display text; may change between versions. */
  name: string;
  /** Coarse grouping for UIs to filter on. Currently always "drum"; an open string so
   * further categories ("synth", "sample", ...) can be added without an API change. */
  category: string;
  params: InstrumentParamInfo[];
}

/** One note for an instrument. */
export interface InstrumentNoteParams {
  /** Absolute runtime time (seconds, as getCurrentTime()). Omitted or already past: now. */
  time?: number;
  /** Level, 0..1 (default 1). */
  velocity?: number;
  /** -1..+1 (default centre). */
  pan?: number;
  /** Semitones, applied on top of the instrument's own Tune (default 0), like a sample
   * event's `pitch`. Pitched instruments (Tom, Kick, Snare, ...) follow it; use it to play
   * them from a tracker note. */
  pitch?: number;
  /** Seconds the note is held before an automatic note-off, counted on the audio thread from
   * the frame the note starts (same convention as a sample event's `duration`). Omitted =
   * held until `instrumentNoteOff()` or until the instrument ends itself. Use it for sustained
   * instruments (Bass); one-shot drums should omit it. */
  duration?: number;
}

/** Max live instrument instances. Mirrors native `kMaxInstruments`. */
export const MAX_INSTRUMENTS = 64;

export interface CaptureHandle {
  id: number;
}

/** Loads/decodes raw audio bytes into PCM ready for the runtime. Implemented with the
 * browser's own decodeAudioData; see src/runtime/decode.ts. */
export type SampleSource = ArrayBuffer;
