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
  /** The command/event protocol version a non-browser host reported (undefined in a browser). See HOST_PROTOCOL_VERSION. */
  hostProtocolVersion?: number;
  /** True when the host plays incoming MIDI notes itself from a route the application declares (AudioRuntime.setMidiRoute). False in a browser. */
  midiRouting?: boolean;
  /** True when the host can play a Sequence itself, locked to its own transport (AudioRuntime.setSequence). False in a browser. */
  nativeSequence?: boolean;
  /** True when the host also plays a Sequence's `loops`: further cycles of their own length, repeating independently (polymeter). */
  sequenceLoops?: boolean;
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
  /** Self-described parameters of every effect kind (ranges, defaults, modulation domain). */
  effects: EffectKindInfo[];
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
  /** Choke group, a positive integer (omit for none). Triggering a voice with a group first stops
   * every active voice of the same group that plays into a *different* `bus`, at this trigger's
   * exact frame, then starts this voice. Voices on the same bus are one source and never choke each
   * other. This is an explicit relationship, separate from voice stealing: it happens whether or
   * not a voice is free. Typical use: open and closed hi-hat on the same group. See
   * docs/choke-groups.md. */
  chokeGroup?: number;
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
  /** Bus-only: the bus this track bus sums into after its own FX chain — another track bus's id
   * or MASTER_BUS (0, default). A self-route, cycle or unknown bus is ignored natively (the old
   * route stays). Set with `setNodeParameter(bus, NodeParam.BusOutput, destBus)`; no effect on
   * MASTER_BUS. See ARCHITECTURE.md, "Buses / mixing". */
  BusOutput = 37,
  /** MASTER_BUS only: master limiter on (1, default) / off (0). See ARCHITECTURE.md, "Master
   * limiter and metering". */
  LimiterEnable = 38,
  /** MASTER_BUS only: limiter ceiling in dBFS (-24..0, default -0.3). */
  LimiterCeiling = 39,
  /** MASTER_BUS only: limiter release time in ms (5..2000, default 100). */
  LimiterRelease = 40,
  /** Utility FX gain, dB (-60 = silence .. +12, default 0 = unity). */
  UtilityGain = 41,
  /** Bus-only: destination track bus of this bus's send, a second output edge tapped after its
   * chain, gain and pan (post-fader), independent of `BusOutput`. 0 = no send (default). A
   * self-send, cycle or unknown bus is ignored natively (the old send stays). See
   * ARCHITECTURE.md, "Bus sends". */
  BusSendTarget = 42,
  /** Bus-only: linear level of the send tap, 0..1 (default 1). Takes effect with a send target. */
  BusSendAmount = 43,
  /** Bus-only: linear level of the main route (`BusOutput`) only, 0..1 (default 1), applied after
   * the send tap. Lets a host silence a bus's main output (solo) without cutting its send. */
  BusMainGain = 44,
  /** Dub Delay division, a step index (see DubDelayDivision): which musical fraction of the
   * transport beat the delay time is. Default 2 (1/8 dotted). Not modulatable. */
  DubDelayDivision = 45,
  /** Dub Delay feedback (0..0.95, default 0.45). */
  DubDelayFeedback = 46,
  /** Dub Delay tone: low-pass inside the feedback loop, Hz (200..16000, default 3000). Each
   * repeat is filtered once more than the last, so echoes darken as they fade. */
  DubDelayTone = 47,
  /** Dub Delay dry/wet (0..1). 0 = bypass. */
  DubDelayMix = 48,
  /** EQ FX band parameters, 7 per band at 49 + 7*(band-1) + field (band 1..4). On 0/1 (default 0),
   * Type (EqBandType), Freq Hz 20..20000, Gain dB -18..18, Q 0.1..18 (bell/notch), ShelfSlope 0.1..1
   * (shelves), Slope 12|24 dB/oct (low-pass/high-pass). Use `eqBandParam()` to address them. */
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
  /** Glitch FX dry/wet (0..1; Gate mode: depth). 0 = bypass; the 0 -> >0 edge engages the effect. */
  GlitchMix = 77,
  /** Glitch FX mode (`GlitchMode`), read at the moment the effect engages. */
  GlitchMode = 78,
  /** Glitch FX division 0..5: 1/2, 1/4, 1/8, 1/16, 1/32, 1/64 of the transport beat grid. */
  GlitchDivision = 79,
  /** Glitch FX read rate -1..2 (loop or tape): 1 untouched, 0 stopped, negative backwards. */
  GlitchSpeed = 80,
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
  /** Minimal gain stage (NodeParam.UtilityGain, dB); the building block for volume automation. */
  Utility = 8,
  /** Tempo-synchronised feedback delay with a low-pass in the feedback loop (DubDelay*
   * params). Its time follows the transport BPM. */
  DubDelay = 9,
  /** Four-band parametric EQ (EqBand* params; see `EqBandType` and `eqResponseDb` for drawing its
   * curve). Inert until a band is enabled. */
  Eq = 10,
  /** Ring-buffer performance effect (Glitch* params): beat repeat, tape stop / slow / reverse, tempo-synced gate.
   * Always records; inert (an exact bypass) until GlitchMix > 0. See docs/glitch-fx.md. */
  Glitch = 11,
}

/** Value space for NodeParam.GlitchMode. */
export enum GlitchMode {
  /** Captures the last division of audio when engaged and loops it (speed reads it slower, faster or backwards). */
  Repeat = 0,
  /** Reads the live signal back through the buffer at `speed`: 1 untouched, 0 a stopped tape, negative backwards. */
  Tape = 1,
  /** Chops the signal with a division-long square, open first. */
  Gate = 2,
}

/** Value space for the EQ band Type params (NodeParam.EqBand1Type...). */
export enum EqBandType {
  Bell = 0,
  LowShelf = 1,
  HighShelf = 2,
  LowPass = 3,
  HighPass = 4,
  Notch = 5,
}

/** Value space for NodeParam.DubDelayDivision: the delay time as a fraction of the transport
 * beat. Values are table indices, in sixteenth notes 1, 2, 3, 4, 6, 8, then the triplets 2/3, 4/3, 8/3, 16/3. */
export enum DubDelayDivision {
  Sixteenth = 0,
  Eighth = 1,
  DottedEighth = 2,
  Quarter = 3,
  DottedQuarter = 4,
  Half = 5,
  SixteenthTriplet = 6,
  EighthTriplet = 7,
  QuarterTriplet = 8,
  HalfTriplet = 9,
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

/** Parameters every instrument kind shares, addressed through setInstrumentParameter() exactly like
 * a kind's own controls. Mirrors native/src/instrument.h `kInstrumentGainParam` (pinned by
 * test/paramIds.test.ts); getInstrumentParams(kind) lists it as the last row, named "gain".
 *  - Gain: the source gain in dB, -24..+12, default 0 (unity — bit-identical to never setting it).
 *    Applied to the instrument's output before it is summed into its bus, so before the track's FX
 *    chain and before the bus fader (NodeParam.BusGain). The sample counterpart is
 *    SampleParam.Gain (linear). Not an LFO target. */
export enum InstrumentCommonParam {
  Gain = 1000,
}

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
  Hypersaw = 9,
  PwmBeast = 10,
  /** Digital Machines: one kind per Machine, one shared FM kernel (docs/digital-machines.md). */
  DmThump = 11,
  DmMetal = 12,
  DmTone = 13,
  DmCrack = 14,
  DmChaos = 15,
  /** Two oscillators + noise + filter + drive: a synthesizer that is exceptionally good at percussion. */
  PercSynth = 16,
  /** Clean, sharp, transient-focused plucks, stabs and leads: band-limited oscillator with enveloped brightness, signed pitch transient, two filter stages with routing. */
  Edge = 17,
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
  /** 0..1 — how far each note's (exponential, acid-style) filter envelope opens the cutoff, 0..5 octaves. */
  EnvMod = 11,
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
  /** Step -1..5 — the pumper's own cycle (same steps as `Rate`), independent of the repeater.
   * Below 0 (default -1) it follows `Rate`. */
  PumpRate = 9,
  /** 0..1 — the pump's recovery shape (not a synth attack): 0 = sharp duck, 1 = long swell. */
  PumpAttack = 10,
  /** 0..1 — per-note pluck: filter snap, pitch click and amp decay to a body. 0 = off. */
  Pluck = 11,
  /** 0..1 — pre-filter drive: 0 clean, 0.5 (default) the classic sound, 1 gritty. */
  Grit = 12,
}

/** PWM Beast controls. Mirrors native `PwmBeastParam`. A pulse-width-modulated character synth.
 * Sustained: pass `duration` on notes (or call `instrumentNoteOff`) to end them. */
export enum PwmBeastParam {
  /** Hz — root; note `pitch` is semitones on top. */
  Tune = 0,
  /** 0.05..0.95 — static pulse width (0.5 = square). */
  Width = 1,
  /** 0..1 — PWM depth around `Width`. */
  Pwm = 2,
  /** Hz — PWM LFO rate. */
  Rate = 3,
  /** 0..1 — PWM source: sine -> skewed ramp (0.5) -> note-envelope sweep (1). */
  Shape = 4,
  /** 0..1 — character stage: clean pulse -> rounded, folded, asymmetric. */
  Harmonics = 5,
  /** 0..1 — asymmetric drive after the character stage (gain-compensated). */
  Drive = 6,
  /** Hz — low-pass cutoff (keytracked). */
  Cutoff = 7,
  /** 0..1 — low-pass resonance. */
  Resonance = 8,
  /** 0..1 — how far the internal timbre envelope opens the filter. */
  EnvAmount = 9,
  /** 0..1 — sub sine (-1 octave) level. */
  Sub = 10,
  /** Seconds. */
  Attack = 11,
  /** Seconds. */
  Decay = 12,
  /** 0..1 — held level. */
  Sustain = 13,
  /** Seconds. */
  Release = 14,
}

/** Digital Machine "Thump" controls (kick / tom / tuned percussion / FM bass hit). Mirrors native
 * `DmThumpParam`. Every Digital Machine puts Tune, Decay and its depth control at ids 0, 1, 2. */
export enum DmThumpParam {
  /** Hz — resting pitch; note `pitch` is semitones on top. */
  Tune = 0,
  /** Seconds — time to fall ~40 dB. */
  Decay = 1,
  /** 0..1 — modulation depth. */
  Fm = 2,
  /** Octaves — pitch-envelope depth (starts this far above Tune). */
  Sweep = 3,
  /** 0..1 — transient: click level and a fast index burst. */
  Punch = 4,
  /** 0..1 — modulator ratio, magnetic over 0.5 1 1.59 2 2.76 3.5 5.4. */
  Ratio = 5,
  /** 0..1 — membrane overtone level and modulator feedback. */
  Body = 6,
  /** 0..1. */
  Drive = 7,
}

/** Digital Machine "Metal" controls (hats / cymbals / clangs / bars and bells). Mirrors native `DmMetalParam`. */
export enum DmMetalParam {
  /** Hz — scales the whole cluster. */
  Tune = 0,
  /** Seconds. */
  Decay = 1,
  /** 0..1 — cross-modulation depth. */
  Fm = 2,
  /** 0..1 — ratio set: harmonic -> bar -> 808 -> scattered (magnetic). */
  Spread = 3,
  /** 0..1 — high-pass tilt (0 = full-range bell, 1 = hat). */
  Tone = 4,
  /** 0..1 — noise level and noise phase-modulation. */
  Noise = 5,
  /** 0..1 — short bright transient on top of the body. */
  Chick = 6,
  /** 0..1. */
  Drive = 7,
}

/** Digital Machine "Tone" controls (four-operator pluck / bass / keys / bell / lead / chord stab).
 * Mirrors native `DmToneParam`. With `Hold` > 0 notes are sustained: pass `duration` (or call
 * `instrumentNoteOff`) to end them. */
export enum DmToneParam {
  /** Hz — root; note `pitch` is semitones on top. */
  Tune = 0,
  /** Seconds. */
  Decay = 1,
  /** 0..1 — modulation depth. */
  Fm = 2,
  /** Step 0..5 — Stack, Twin, Fan, Major, Minor, Sus (the last three are chord stabs). */
  Structure = 3,
  /** 0..1 — modulator ratio, magnetic over 0.5 1 1.41 2 2.76 3 3.5 4 5 7. */
  Ratio = 4,
  /** 0..1 — how long the modulators outlive the attack (0 = pluck). */
  Damp = 5,
  /** Seconds. */
  Attack = 6,
  /** 0..1 — held level (0 = pluck, 1 = sustained until note-off). */
  Hold = 7,
  /** 0..1. */
  Drive = 8,
}

/** Digital Machine "Crack" controls (snare / clap / rim / noisy percussion). Mirrors native `DmCrackParam`. */
export enum DmCrackParam {
  /** Hz — body pitch. */
  Tune = 0,
  /** Seconds. */
  Decay = 1,
  /** 0..1 — modulation depth, including how hard noise phase-modulates the body. */
  Fm = 2,
  /** 0..1 — FM body level. */
  Body = 3,
  /** 0..1 — noise level. */
  Noise = 4,
  /** 0..1 — 1 burst (snare) -> 5 bursts (clap); also fades the body. */
  Burst = 5,
  /** 0..1 — noise band-pass centre. */
  Tone = 6,
  /** 0..1. */
  Drive = 7,
}

/** Digital Machine "Chaos" controls (unstable feedback-ring FM: strange digital sounds, metallic
 * leads). Mirrors native `DmChaosParam`. With `Hold` > 0 notes are sustained: pass `duration` (or
 * call `instrumentNoteOff`) to end them. */
export enum DmChaosParam {
  /** Hz — root; note `pitch` is semitones on top. */
  Tune = 0,
  /** Seconds. */
  Decay = 1,
  /** 0..1 — ring loop gain: stable -> chaotic (output is always bounded). */
  Chaos = 2,
  /** 0..1 — ring operator ratio, magnetic. */
  Ratio = 3,
  /** 0..1 — wavefold. */
  Fold = 4,
  /** 0..1 — noise injected into the ring. */
  Noise = 5,
  /** Octaves — pitch-envelope depth. */
  Sweep = 6,
  /** 0..1 — held level (0 = one-shot, 1 = sustained until note-off). */
  Hold = 7,
}

/** Percussion Synth controls. Mirrors native `PercSynthParam` (docs/percussion-synth.md). Two
 * oscillators and noise through a filter and a drive stage; one pitch sweep. With `Sustain` > 0
 * notes are held: pass `duration` (or call `instrumentNoteOff`) to end them. */
export enum PercSynthParam {
  /** Hz — oscillator 1 resting pitch; note `pitch` is semitones on top (oscillator 2 follows). */
  Tune = 0,
  /** Step 0..2 — sine/triangle, saw, square. */
  Osc1Wave = 1,
  /** 0..1. */
  Osc1Level = 2,
  /** Step 0..2 — sine/triangle, saw, square. */
  Osc2Wave = 3,
  /** Semitones relative to oscillator 1; fractions give inharmonic / metallic tones. */
  Osc2Pitch = 4,
  /** 0..1. */
  Osc2Level = 5,
  /** 0..1 — oscillator 1 -> oscillator 2 frequency modulation (works with oscillator 1 level at 0). */
  Osc2Fm = 6,
  /** Octaves — how far above the resting pitch oscillator 1 starts. */
  PitchEnv = 7,
  /** Seconds — time for the sweep to fall ~40 dB. */
  PitchDecay = 8,
  /** 0..2 — how much of the sweep oscillator 2 follows (× `PitchEnv`). */
  Osc2Env = 9,
  /** 0..1 — noise level. */
  Noise = 10,
  /** 0..1 — noise colour: dark (0), white (0.5), bright (1). */
  NoiseTone = 11,
  /** Seconds — noise envelope, time to fall ~40 dB. */
  NoiseDecay = 12,
  /** Step 0/1 — low-pass, high-pass. */
  FilterType = 13,
  /** Hz — filter cutoff (keytracked). */
  Cutoff = 14,
  /** 0..1. */
  Resonance = 15,
  /** -1..1 — filter envelope depth (±6 octaves). */
  FilterEnv = 16,
  /** Seconds — filter envelope, time to fall ~40 dB. */
  FilterDecay = 17,
  /** Seconds. */
  Attack = 18,
  /** Seconds. */
  Decay = 19,
  /** 0..1 — held level (0 = one-shot). */
  Sustain = 20,
  /** Seconds. */
  Release = 21,
  /** 0..1 — clean -> saturated -> aggressive. */
  Drive = 22,
}

/** Edge controls. Mirrors native `EdgeParam` (docs/edge-instrument.md). A band-limited oscillator
 * whose brightness is a harmonic count (enveloped), a signed pitch transient, and two filter stages
 * with routing. With `Sustain` > 0 notes are held: pass `duration` (or call `instrumentNoteOff`) to
 * end them. */
export enum EdgeParam {
  /** Hz — root; note `pitch` is semitones on top. */
  Tune = 0,
  /** 0..1 — saw (0), square (0.5), narrow pulse (1). */
  Shape = 1,
  /** 0..1 — oscillator brightness: harmonic count from 1 (sine) to 256. */
  Bright = 2,
  /** -1..1 — tone envelope depth on brightness (positive: a bright onset that settles). */
  BrightEnv = 3,
  /** Cents — second oscillator, fades in over the first 10 cents (0 = single). */
  Detune = 4,
  /** Semitones, signed — pitch transient start offset (positive: from above). */
  PitchEnv = 5,
  /** Seconds — time for the transient to fall ~40 dB. */
  PitchDecay = 6,
  /** Step 0..2 — filter A only, series (A -> B), parallel (A + B). */
  Routing = 7,
  /** Step 0..3 — LP 12 dB, LP 24 dB, HP 12 dB, band-pass. */
  FilterA = 8,
  /** Step 0..3 — same modes as A. */
  FilterB = 9,
  /** Hz — filter A cutoff (keytracked). */
  Cutoff = 10,
  /** Octaves, signed — filter B cutoff relative to A. */
  Spread = 11,
  /** 0..1 — shared; in series B takes a fraction of A's. */
  Resonance = 12,
  /** -1..1 — tone envelope depth on both cutoffs (±6 octaves). */
  FilterEnv = 13,
  /** Seconds — tone envelope (filter + brightness), time to fall ~40 dB. */
  ToneDecay = 14,
  /** Hz. */
  LfoRate = 15,
  /** 0..1 — LFO -> pitch (vibrato), up to ±1.5 semitones. */
  LfoPitch = 16,
  /** 0..1 — LFO -> both cutoffs, up to ±3 octaves. */
  LfoFilter = 17,
  /** Seconds. */
  Attack = 18,
  /** Seconds. */
  Decay = 19,
  /** 0..1 — held level (0 = one-shot). */
  Sustain = 20,
  /** Seconds. */
  Release = 21,
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

/** Hypersaw controls. Mirrors native `HypersawParam`. Seven detuned saws as one voice. Sustained:
 * pass `duration` on notes (or call `instrumentNoteOff`) to end them. */
export enum HypersawParam {
  /** Hz — root; note `pitch` is semitones on top. */
  Tune = 0,
  /** Cents — detune of the outermost saw pair (the others scale from it). */
  Detune = 1,
  /** 0..1 — stereo width (0 = all centred, 1 = full distribution; widens with detune). */
  Spread = 2,
  /** 0..1 — centre-vs-sides mix: lone centre saw -> full stack. */
  Density = 3,
  /** Hz — low-pass cutoff (keytracked). */
  Cutoff = 4,
  /** Seconds. */
  Attack = 5,
  /** Seconds. */
  Decay = 6,
  /** 0..1 — held level. */
  Sustain = 7,
  /** Seconds. */
  Release = 8,
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

/** Self-description of one effect parameter. Same shape as InstrumentParamInfo, except `id` is a
 * NodeParam and `name` is the stable string an application addresses it by ("cutoff", "mix"). */
export type EffectParamInfo = InstrumentParamInfo;

/** One effect kind and its parameters, reported by the engine. */
export interface EffectKindInfo {
  kind: EffectKind;
  params: EffectParamInfo[];
}

/** What a scheduled parameter event writes to: an effect instance, an instrument, a sample, or a bus's mixer
 * parameters (`param` is then BusGain, BusPan, BusSendAmount or BusMainGain; routing is not schedulable).
 * Bus targets need a native host that implements protocol 4 (`capabilities.hostProtocolVersion`): the browser
 * worklet does not take them, and `scheduleParams` drops them there. */
export type ScheduledParamTarget =
  | { effect: { bus: BusId; effect: EffectId } }
  | { instrument: InstrumentId }
  | { sample: SampleId }
  | { bus: BusId };

/** A timestamped write of a parameter's BASE value (engine LFOs keep riding on it), optionally
 * ramping linearly. See AudioRuntime.scheduleParams() and docs/scheduled-params.md. */
export interface ScheduledParam {
  /** Audio-clock seconds, as ScheduledEvent.time. A time in the past applies on the next block. */
  time: number;
  target: ScheduledParamTarget;
  /** The id the matching immediate setter takes: a NodeParam (effect), the synth's param id
   * (instrument), or a SampleParam (sample). */
  param: number;
  /** The value reached: at `time` for a step, at `time + ramp` for a ramp. */
  value: number;
  /** Ramp start value. Omitted: the ramp starts from the parameter's current base value. With
   * it, the event is self-contained (independent of what the engine last did). */
  from?: number;
  /** Ramp length in seconds (default 0 = a step). Linear in the parameter's own units. */
  ramp?: number;
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

/** One LFO routed to one effect parameter (`target` is a NodeParam). Same fields and semantics as
 * InstrumentLfo, minus `retrigger` (an effect has no notes). */
export type EffectLfo = Omit<InstrumentLfo, "retrigger">;

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
  /** Choke group, a positive integer (omit for none): same rules as `TriggerParams.chokeGroup`,
   * across sample voices and instruments alike. Starting this note first cuts every sounding
   * source of the group that plays into a different bus. The instrument's own bus is its source
   * identity. See docs/choke-groups.md. */
  chokeGroup?: number;
  /** A tied note (303-style slide): it glides into the note still held on this instrument, with no
   * re-attack, whatever the instrument's own Legato switch says. Honoured by instruments with a legato
   * mode (Bass, FM); for the rest it is an ordinary retrigger. The held note must still be sounding
   * when this one starts, so give it a `duration` that reaches past this note's start. */
  tie?: boolean;
}

/** Max live instrument instances. Mirrors native `kMaxInstruments`. */
export const MAX_INSTRUMENTS = 64;

export interface CaptureHandle {
  id: number;
}

/** Loads/decodes raw audio bytes into PCM ready for the runtime. Implemented with the
 * browser's own decodeAudioData; see src/runtime/decode.ts. */
export type SampleSource = ArrayBuffer;
