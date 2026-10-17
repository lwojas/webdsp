/** Opaque identifier for a sample resource held by the runtime. */
type SampleId = number;
/** Opaque handle for a single triggered voice. Not guaranteed to stay valid forever —
 * voices can be stolen or finish playback; operations on a dead handle are no-ops. */
type VoiceHandle = number;
/** Opaque identifier for a mixer bus (e.g. the default "master" bus). */
type BusId = number;
declare const MASTER_BUS: BusId;
/** How many buses beyond MASTER_BUS an application can allocate via
 * `AudioRuntime.createBus()` — e.g. one per sequencer track, each with its own filter+delay
 * chain (see NodeParam) that sums into MASTER_BUS before MASTER_BUS's own chain runs.
 * Mirrors native/src/engine.h's `kMaxTrackBuses` — keep in sync (see that file's comment for
 * why a mismatch is safe either way, just wasteful or overly restrictive). 64 covers 4 banks of
 * 16 pads (ECS-84). */
declare const MAX_TRACK_BUSES = 64;
/** Metadata the runtime exposes for a loaded sample. Mirrors native/src/sample_store.h. */
interface SampleMetadata {
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
}
/** Runtime/hardware capabilities and current policy limits. Not necessarily fixed hardware
 * facts — maxVoices in particular is a runtime policy that can adapt to the device. */
interface RuntimeCapabilities {
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
interface RuntimeDiagnostics {
    activeVoices: number;
    loadedSamples: number;
    sampleMemoryBytes: number;
}
/** Real-time-safe voice playback parameters. All optional; omitted fields keep the
 * sample's natural defaults (full length, unity gain/rate, no loop). */
interface TriggerParams {
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
interface ScheduledEvent extends TriggerParams {
    time: number;
}
declare enum VoiceParam {
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
    Pan = 6
}
declare enum SampleParam {
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
    Pan = 5
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
interface EnvelopeParams {
    /** Seconds to rise from silence to peak. */
    attack?: number;
    /** Seconds to fall from peak to the sustain level. */
    decay?: number;
    /** Held level, 0..1. */
    sustain?: number;
    /** Seconds to fall from the level at note-off to silence. */
    release?: number;
}
declare enum NodeParam {
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
    CrushMix = 36
}
/** Opaque identifier of one effect instance in a bus's FX chain, returned by
 * `AudioRuntime.addEffect()`. Unique per runtime, so `filter` (an EffectKind) and the two
 * filters you added (two EffectIds) are different things. */
type EffectId = number;
/** The kind of DSP an effect instance runs. Mirrors native/src/fx_chain.h `FxKind` (pinned by
 * test/paramIds.test.ts). Any number of instances of each kind may be in one chain. A kind's
 * parameters are the NodeParam ids with its prefix (FilterCutoff.., DelayTime.., ...). */
declare enum EffectKind {
    Filter = 0,
    ChorusFlanger = 1,
    Delay = 2,
    Reverb = 3,
    Compressor = 4,
    Saturation = 5,
    Phaser = 6,
    Bitcrusher = 7
}
/** One entry of a bus's FX chain, as returned by `AudioRuntime.getEffectChain()`. */
interface EffectInfo {
    id: EffectId;
    kind: EffectKind;
    enabled: boolean;
}
/** Max effect instances per bus chain. Mirrors native `kMaxFxPerBus`. */
declare const MAX_EFFECTS_PER_BUS = 16;
/** Shared value space for VoiceParam.FilterMode / NodeParam.FilterMode — not itself a
 * param id, just what the 0/1 float value passed to setVoiceParameter/setNodeParameter
 * means for either filter instance. */
declare enum FilterMode {
    LowPass = 0,
    HighPass = 1
}
/** Value space for VoiceParam.FilterSlope / NodeParam.FilterSlope: the rolloff in dB/octave.
 * 12 = two-pole (default, the original behavior), 24 = four-pole. */
declare enum FilterSlope {
    Db12 = 12,
    Db24 = 24
}
/** Opaque identifier of one instrument instance, returned by `AudioRuntime.createInstrument()`. */
type InstrumentId = number;
/** Kinds of synthesized instrument. Mirrors native/src/params.h `InstrumentKind` (pinned by
 * test/paramIds.test.ts). Each kind is a purpose-built machine with its own small parameter
 * set — not a generic synth. */
declare enum InstrumentKind {
    Kick = 0,
    Snare = 1,
    Clap = 2,
    Tom = 3,
    HiHat = 4,
    Bass = 5,
    Pulse = 6,
    Pad = 7,
    Fm = 8
}
/** Kick controls. Mirrors native `KickParam`. Ranges/defaults/units come from
 * `getInstrumentParams(InstrumentKind.Kick)` so UIs need not hardcode them. */
declare enum KickParam {
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
    Drive = 5
}
/** Snare controls. Mirrors native `SnareParam`. */
declare enum SnareParam {
    /** Hz — body pitch. */
    Tune = 0,
    /** Seconds — overall length. */
    Decay = 1,
    /** 0..1 — transient + brightness of the noise. */
    Snap = 2,
    /** 0..1 — noise vs tonal body balance. */
    Noise = 3
}
/** Clap controls. Mirrors native `ClapParam`. */
declare enum ClapParam {
    /** Hz — centre of the noise band. */
    Tune = 0,
    /** Seconds — length of the final tail. */
    Decay = 1,
    /** 0..1 — time between the burst cluster's hits. */
    Spread = 2,
    /** 0..1 — sharpness of the bursts. */
    Snap = 3
}
/** Tom controls. Mirrors native `TomParam`. */
declare enum TomParam {
    /** Hz — resting fundamental (note `pitch` offsets it in semitones). */
    Tune = 0,
    /** Octaves above Tune the strike starts. */
    PitchDrop = 1,
    /** Seconds — ring length. */
    Decay = 2,
    /** 0..1 — membrane overtones + stick tick. */
    Body = 3,
    /** 0..1 — damping: shorter ring, darker tone. */
    Damp = 4
}
/** Hi-hat controls. Mirrors native `HiHatParam`. */
declare enum HiHatParam {
    /** Semitones — offset of the metallic cluster. */
    Tune = 0,
    /** Seconds — short = closed, long = open. */
    Decay = 1,
    /** 0..1 — noise -> inharmonic metallic cluster. */
    Metal = 2,
    /** 0..1 — brightness. */
    Tone = 3
}
/** Bass controls. Mirrors native `BassParam`. Sustained: pass `duration` on notes (or call
 * `instrumentNoteOff`) to end them. */
declare enum BassParam {
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
    Slide = 10
}
/** Pulse controls. Mirrors native `PulseParam`. Pump and repeat are locked to the engine's beat
 * grid (see `AudioRuntime.setTempo()`), not to note starts. Sustained: pass `duration` on notes
 * (or call `instrumentNoteOff`) to end them. */
declare enum PulseParam {
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
    Release = 8
}
/** Pad controls. Mirrors native `PadParam`. Sustained and free-running (not tempo-synced): pass
 * `duration` on notes (or call `instrumentNoteOff`) to end them. A new note on a sounding instance
 * moves the previous note into its release, so successive notes overlap. */
declare enum PadParam {
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
    Release = 6
}
/** FM controls. Mirrors native `FmParam`. Sustained: pass `duration` on notes (or call
 * `instrumentNoteOff`) to end them. `legato` >= 0.5 makes a note on a held note continue the voice. */
declare enum FmParam {
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
    Slide = 10
}
/** How the engine's LFO layer may modulate a parameter, declared by the synth that owns it:
 * "none" = not a modulation target; "linear" = moves in its own units (100% = half the range
 * either side of the base); "log" = moves in octaves (Hz-valued controls). */
type ModulationDomain = "none" | "linear" | "log";
/** Self-description of one instrument control. */
interface InstrumentParamInfo {
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
declare enum LfoWaveform {
    Sine = 0,
    Triangle = 1,
    Square = 2,
    Saw = 3
}
/** One LFO routed to one instrument parameter. See docs/lfo-modulation.md. */
interface InstrumentLfo {
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
interface ModulationCapabilities {
    /** LFO slots per instrument (slot indices 0..maxLfos-1). */
    maxLfos: number;
    /** Waveform display names, indexed by `LfoWaveform`. */
    waveforms: string[];
    /** Sync divisions, slowest first; `beats` = cycle length in quarter-note beats. */
    divisions: {
        name: string;
        beats: number;
    }[];
    minRateHz: number;
    maxRateHz: number;
}
interface InstrumentKindInfo {
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
interface InstrumentNoteParams {
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
declare const MAX_INSTRUMENTS = 64;
interface CaptureHandle {
    id: number;
}
/** Loads/decodes raw audio bytes into PCM ready for the runtime. Implemented with the
 * browser's own decodeAudioData; see src/runtime/decode.ts. */
type SampleSource = ArrayBuffer;

/** Sync divisions, slowest first (same order as `ModulationCapabilities.divisions`). `beats` is
 * the cycle length in quarter-note beats. */
declare const LFO_DIVISIONS: readonly {
    name: string;
    beats: number;
}[];
/** Waveform value in [-1, 1] for a phase (any real; wrapped to [0, 1)). Sine and triangle start
 * at 0 and rise, saw ramps -1 -> +1, square is +1 for the first half. */
declare function lfoShape(waveform: LfoWaveform, phase: number): number;
interface LfoEvalSettings {
    waveform: LfoWaveform;
    /** false: free-running at `rateHz`; true: locked to the beat grid at `division`. */
    sync: boolean;
    /** Free mode, Hz. */
    rateHz: number;
    /** Sync mode: index into LFO_DIVISIONS. */
    division: number;
    /** Phase offset, 0..1 (default 0). */
    phase?: number;
}
interface LfoEvalContext {
    /** The absolute time to evaluate at, seconds (e.g. an event's scheduled time). */
    time: number;
    /** Time of the beat grid's anchor (AudioRuntime.setTempo's anchorTime). Cycle 0 starts here, in
     * both modes. */
    anchorTime: number;
    /** Tempo of the beat grid; a beat is a quarter note. Sync mode only. */
    bpm: number;
}
/** LFO output in [-1, 1] at `ctx.time`. A pure function of its arguments: the same settings and
 * context always give the same value, regardless of when it is called. */
declare function lfoValueAt(s: LfoEvalSettings, ctx: LfoEvalContext): number;

interface AudioRuntimeOptions {
    /** Reuse an existing AudioContext (e.g. one the host app already created for other
     * purposes) instead of letting the runtime create its own. */
    audioContext?: AudioContext;
    /** Initial voice pool size. A runtime policy, not a hardware limit — see
     * getCapabilities() and ARCHITECTURE.md, "Voices, not channels". */
    maxVoices?: number;
    /** Fetchable URL to the built engine-processor.js (native/build.sh produces the source
     * this compiles from). No default is provided — where this file is actually servable
     * from is up to your app's bundler/host. The common case: `import { defaultWorkletUrl }
     * from "webdsp/worklet-url"`. See ARCHITECTURE.md, "Using this as a package". */
    workletModuleUrl: string | URL;
}
/**
 * The stable public boundary between an application/UI and the audio runtime. Everything
 * on this class deals in samples, voices, buses, and scheduled events — never pads,
 * patterns, or any other application concept. See ARCHITECTURE.md for the full boundary
 * rationale and what's intentionally left out of this surface for v1.
 */
declare class AudioRuntime {
    private readonly ctx;
    private readonly bridge;
    private readonly capabilities;
    private nextSampleId;
    private nextVoiceId;
    private nextCaptureId;
    private nextTrackBus;
    private nextEffectId;
    private nextInstrumentId;
    private readonly instruments;
    private readonly effectChains;
    private readonly samples;
    private diagnostics;
    private readonly diagnosticsListeners;
    private readonly voiceEndedListeners;
    private readonly pendingCaptures;
    private readonly pendingArmedCaptures;
    private constructor();
    static create(options: AudioRuntimeOptions): Promise<AudioRuntime>;
    resume(): Promise<void>;
    suspend(): Promise<void>;
    close(): Promise<void>;
    getCurrentTime(): number;
    getCapabilities(): RuntimeCapabilities;
    getDiagnostics(): RuntimeDiagnostics;
    onDiagnostics(fn: (d: RuntimeDiagnostics) => void): () => void;
    onVoiceEnded(fn: (voice: VoiceHandle) => void): () => void;
    loadSample(data: ArrayBuffer, opts?: {
        name?: string;
        gain?: number;
        pan?: number;
        envelope?: EnvelopeParams;
    }): Promise<SampleMetadata>;
    removeSample(id: SampleId): void;
    getSample(id: SampleId): SampleMetadata | undefined;
    listSamples(): SampleMetadata[];
    /** Triggers a sample. Returns a handle immediately (see ARCHITECTURE.md, "Voice
     * handles") even though the actual audio-thread command is delivered asynchronously. */
    trigger(params: TriggerParams): VoiceHandle;
    release(voice: VoiceHandle): void;
    stop(voice: VoiceHandle): void;
    setVoiceParameter(voice: VoiceHandle, param: VoiceParam, value: number): void;
    /** Sets a persistent parameter on the sample itself (SampleParam.Gain, SampleParam.Pan) — not on any
     * one playing voice. Takes effect immediately for every voice currently playing that
     * sample, not just ones triggered afterward (see native/src/voice.h#render). A no-op if
     * `sample` doesn't currently reference a loaded sample. */
    setSampleParameter(sample: SampleId, param: SampleParam, value: number): void;
    /** Sets the amplitude ADSR envelope (see EnvelopeParams for units, ranges and behaviour) of
     * every voice that plays `sample`. Only the fields present are changed. A no-op if `sample`
     * doesn't reference a loaded sample. */
    setSampleEnvelope(sample: SampleId, envelope: EnvelopeParams): void;
    setNodeParameter(bus: BusId, param: NodeParam, value: number): void;
    /** Adds an instance of `kind` to `bus`'s chain at `index` (default: the end) and returns its
     * id. The new instance has its own state and parameters, and is inert until parameterized
     * with setEffectParameter(). `enabled: false` adds it bypassed. Throws if the chain already
     * holds MAX_EFFECTS_PER_BUS instances. */
    addEffect(bus: BusId, kind: EffectKind, opts?: {
        index?: number;
        enabled?: boolean;
    }): EffectId;
    /** Removes the instance and frees its state. Other instances — including others of the same
     * kind — are untouched. A no-op for an unknown id. */
    removeEffect(bus: BusId, effect: EffectId): void;
    /** Moves the instance to `index` (clamped), keeping its state. */
    moveEffect(bus: BusId, effect: EffectId, index: number): void;
    /** Bypasses (false) or re-enables (true) one instance. Bypass clears its internal state. */
    setEffectEnabled(bus: BusId, effect: EffectId, enabled: boolean): void;
    /** Sets a parameter of one instance, using the same NodeParam ids as setNodeParameter()
     * (e.g. NodeParam.FilterCutoff on a Filter instance). A parameter that doesn't belong to the
     * instance's kind is ignored. */
    setEffectParameter(bus: BusId, effect: EffectId, param: NodeParam, value: number): void;
    /** Snapshot of the instances added through addEffect() on `bus`, in processing order. The
     * legacy setNodeParameter() API creates its own instances inside the engine; those are not
     * listed here. */
    getEffectChain(bus: BusId): readonly EffectInfo[];
    private chainFor;
    /** Parameter descriptions (id, name, min, max, default, unit) for `kind`, straight from the
     * engine — enough to build controls without knowing what they do inside. */
    getInstrumentParams(kind: InstrumentKind): readonly InstrumentParamInfo[];
    /** Creates an instrument instance of `kind` feeding `opts.bus` (default MASTER_BUS) and
     * returns its id synchronously. One instance is one monophonic machine: a new note restarts
     * it. Create one per part that must overlap. Throws past MAX_INSTRUMENTS or for an unknown kind. */
    createInstrument(kind: InstrumentKind, opts?: {
        bus?: BusId;
    }): InstrumentId;
    /** Frees the instance. Pending scheduled notes for it are dropped. No-op for an unknown id. */
    removeInstrument(instrument: InstrumentId): void;
    /** Plays a note. With a future `time` it is scheduled sample-accurately (same clock as
     * schedule()); cancelScheduled() cancels it too. */
    instrumentNoteOn(instrument: InstrumentId, params?: InstrumentNoteParams): void;
    /** LFO limits and option lists (slots per instrument, waveform names, sync divisions). */
    getModulationInfo(): ModulationCapabilities;
    /** The params of `kind` an LFO can target, straight from the synth's own metadata. Build a
     * modulation target picker from this; never hardcode target lists in an application. */
    getModulationTargets(kind: InstrumentKind): readonly InstrumentParamInfo[];
    /** Enables or edits LFO `slot` (0..maxLfos-1) on an instrument. Editing keeps the LFO's phase
     * running (only a disabled -> enabled change restarts it). An invalid target (not a modulation
     * target of the instrument's kind) is rejected by the engine with an "error" event and leaves
     * the slot off. Free-mode LFOs are independent of the transport; synced ones follow setTempo(). */
    setInstrumentLfo(instrument: InstrumentId, slot: number, lfo: InstrumentLfo): void;
    /** Removes LFO `slot`; its target goes back to the plain base value. */
    clearInstrumentLfo(instrument: InstrumentId, slot: number): void;
    /** Chokes the instrument with a short release. Optional for one-shot drums. */
    instrumentNoteOff(instrument: InstrumentId): void;
    /** Sets one of the instrument's own controls (ids from its kind's enum, e.g. KickParam).
     * Values are clamped by the engine to the range in getInstrumentParams(). */
    setInstrumentParameter(instrument: InstrumentId, param: number, value: number): void;
    /** Allocates a new bus — an application-level channel of processing (e.g. one sequencer
     * track's own FX chain) with the same filter+delay capability as MASTER_BUS, summed into
     * MASTER_BUS before MASTER_BUS's own chain runs. Feed it by passing the returned id as
     * `bus` on trigger()/schedule(), and shape it with setNodeParameter(id, ...) exactly like
     * MASTER_BUS. No worklet round-trip: the engine pre-allocates MAX_TRACK_BUSES track buses
     * at init — each a no-op pass-through until parameterized — so this is a synchronous,
     * real-time-safe counter bump, not a command. Throws once MAX_TRACK_BUSES are handed out. */
    createBus(): BusId;
    /** Compiles a batch of generic scheduled events (as an external sequencer would produce —
     * see src/sequencing for an example) into voice triggers at the given absolute engine
     * times. Returns one handle per event, in order, for later release()/stop() calls. */
    schedule(events: ScheduledEvent[]): VoiceHandle[];
    /** Sets the beat grid tempo-aware instruments (Pulse) lock their rhythm to. Beats lie at
     * `anchorTime + n * 60/bpm` seconds (same time domain as getCurrentTime()); `anchorTime`
     * defaults to `time`, or now. `time` is when the change takes effect — pass the time of the
     * step where a tempo change lands so it arrives with the notes scheduled for it (omitted =
     * immediately). For a continuous pulse across a tempo change, anchor at a beat boundary.
     * Not cancelled by cancelScheduled(). Until called the grid is 120 BPM anchored at 0. */
    setTempo(bpm: number, opts?: {
        time?: number;
        anchorTime?: number;
    }): void;
    cancelScheduled(fromTime?: number): void;
    startCapture(bus?: BusId): CaptureHandle;
    /** Stops a capture and resolves once the recorded audio has been registered as a new
     * runtime Sample, ready to trigger() like any other. */
    stopCapture(handle: CaptureHandle): Promise<SampleMetadata>;
    /** Arms a sample-accurate capture of the master bus's processed output over
     * [startTime, stopTime) — absolute engine time, same domain as getCurrentTime()/
     * schedule()'s ScheduledEvent.time. The engine begins and ends the capture on the exact
     * sample regardless of when this call's postMessage is actually delivered, the same way
     * schedule() is sample-accurate regardless of message jitter (see native/src/capture.h's
     * arm()) — there is no "stopCapture" call to make afterward, the engine finishes it on its
     * own and this resolves once that happens.
     *
     * Throws synchronously if [startTime, stopTime) exceeds
     * getCapabilities().maxCaptureSeconds, so a caller gets a deterministic, pre-flight error
     * instead of an asynchronous refusal discovered later. Unlike stopCapture(), the resolved
     * value includes the raw captured PCM (`channelData`, one planar Float32Array-backed
     * ArrayBuffer per channel) alongside the registered Sample's metadata — needed by a caller
     * that wants to persist the capture as more than a live-session-only engine Sample. */
    armCapture(opts: {
        startTime: number;
        stopTime: number;
        bus?: BusId;
    }): {
        handle: CaptureHandle;
        result: Promise<{
            metadata: SampleMetadata;
            channelData: ArrayBuffer[];
        }>;
    };
    private handleWorkletEvent;
}

export { AudioRuntime, type AudioRuntimeOptions, BassParam, type BusId, type CaptureHandle, ClapParam, type EffectId, type EffectInfo, EffectKind, type EnvelopeParams, FilterMode, FilterSlope, FmParam, HiHatParam, type InstrumentId, InstrumentKind, type InstrumentKindInfo, type InstrumentLfo, type InstrumentNoteParams, type InstrumentParamInfo, KickParam, LFO_DIVISIONS, type LfoEvalContext, type LfoEvalSettings, LfoWaveform, MASTER_BUS, MAX_EFFECTS_PER_BUS, MAX_INSTRUMENTS, MAX_TRACK_BUSES, type ModulationCapabilities, type ModulationDomain, NodeParam, PadParam, PulseParam, type RuntimeCapabilities, type RuntimeDiagnostics, type SampleId, type SampleMetadata, SampleParam, type SampleSource, type ScheduledEvent, SnareParam, TomParam, type TriggerParams, type VoiceHandle, VoiceParam, lfoShape, lfoValueAt };
