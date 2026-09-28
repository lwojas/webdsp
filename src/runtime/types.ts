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
 * why a mismatch is safe either way, just wasteful or overly restrictive). */
export const MAX_TRACK_BUSES = 32;

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
}

/** Shared value space for VoiceParam.FilterMode / NodeParam.FilterMode — not itself a
 * param id, just what the 0/1 float value passed to setVoiceParameter/setNodeParameter
 * means for either filter instance. */
export enum FilterMode {
  LowPass = 0,
  HighPass = 1,
}

export interface CaptureHandle {
  id: number;
}

/** Loads/decodes raw audio bytes into PCM ready for the runtime. Implemented with the
 * browser's own decodeAudioData; see src/runtime/decode.ts. */
export type SampleSource = ArrayBuffer;
