/** Opaque identifier for a sample resource held by the runtime. */
type SampleId = number;
/** Opaque handle for a single triggered voice. Not guaranteed to stay valid forever —
 * voices can be stolen or finish playback; operations on a dead handle are no-ops. */
type VoiceHandle = number;
/** Opaque identifier for a mixer bus (e.g. the default "master" bus). */
type BusId = number;
declare const MASTER_BUS: BusId;
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
    FilterMode = 6
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
    private readonly samples;
    private diagnostics;
    private readonly diagnosticsListeners;
    private readonly voiceEndedListeners;
    private readonly pendingCaptures;
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
    /** Compiles a batch of generic scheduled events (as an external sequencer would produce —
     * see src/sequencing for an example) into voice triggers at the given absolute engine
     * times. Returns one handle per event, in order, for later release()/stop() calls. */
    schedule(events: ScheduledEvent[]): VoiceHandle[];
    cancelScheduled(fromTime?: number): void;
    startCapture(bus?: BusId): CaptureHandle;
    /** Stops a capture and resolves once the recorded audio has been registered as a new
     * runtime Sample, ready to trigger() like any other. */
    stopCapture(handle: CaptureHandle): Promise<SampleMetadata>;
    private handleWorkletEvent;
}

export { AudioRuntime, type AudioRuntimeOptions, type BusId, type CaptureHandle, FilterMode, MASTER_BUS, NodeParam, type RuntimeCapabilities, type RuntimeDiagnostics, type SampleId, type SampleMetadata, type SampleSource, type ScheduledEvent, type TriggerParams, type VoiceHandle, VoiceParam };
