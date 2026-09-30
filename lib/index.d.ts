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
 * why a mismatch is safe either way, just wasteful or overly restrictive). */
declare const MAX_TRACK_BUSES = 32;
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
    FilterMode = 4
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
    ChorusFlangerMix = 25
}
/** Shared value space for VoiceParam.FilterMode / NodeParam.FilterMode — not itself a
 * param id, just what the 0/1 float value passed to setVoiceParameter/setNodeParameter
 * means for either filter instance. */
declare enum FilterMode {
    LowPass = 0,
    HighPass = 1
}
interface CaptureHandle {
    id: number;
}
/** Loads/decodes raw audio bytes into PCM ready for the runtime. Implemented with the
 * browser's own decodeAudioData; see src/runtime/decode.ts. */
type SampleSource = ArrayBuffer;

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
    setNodeParameter(bus: BusId, param: NodeParam, value: number): void;
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

export { AudioRuntime, type AudioRuntimeOptions, type BusId, type CaptureHandle, FilterMode, MASTER_BUS, MAX_TRACK_BUSES, NodeParam, type RuntimeCapabilities, type RuntimeDiagnostics, type SampleId, type SampleMetadata, type SampleSource, type ScheduledEvent, type TriggerParams, type VoiceHandle, VoiceParam };
