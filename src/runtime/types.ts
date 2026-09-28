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
export const enum VoiceParam {
  Gain = 0,
  Rate = 1,
  FilterCutoff = 2,
  FilterResonance = 3,
}

// Bus-addressed parameters (v1 has exactly one bus, "master"). Per-voice DSP node
// parameters (currently just the filter) go through setVoiceParameter/VoiceParam instead —
// see native/src/voice.h. A future per-voice node that needs NodeParam-style addressing
// is a natural extension (see ARCHITECTURE.md, "Intentionally deferred"); nothing here
// pre-builds a target union for that until such a node exists.
export const enum NodeParam {
  DelayTime = 0,
  DelayFeedback = 1,
  DelayMix = 2,
  BusGain = 3,
}

export interface CaptureHandle {
  id: number;
}

/** Loads/decodes raw audio bytes into PCM ready for the runtime. Implemented with the
 * browser's own decodeAudioData; see src/runtime/decode.ts. */
export type SampleSource = ArrayBuffer;
