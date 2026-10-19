// Message protocol between the main thread (AudioRuntime) and the AudioWorkletProcessor.
//
// This is the *only* channel that crosses the runtime's realtime boundary. Everything here
// is a plain, structured-cloneable object; large PCM payloads are sent as Transferable
// ArrayBuffers (zero-copy) rather than copied. Commands are infrequent relative to the audio
// render quantum (128 frames @ 48kHz ~= 2.7ms) — a note trigger, a parameter tweak, a batch
// of scheduled events are all tiny messages, so postMessage's per-message overhead is not a
// realtime hazard here. What *would* be a hazard is putting per-sample-frame data on this
// channel, which nothing in this codebase does. See ARCHITECTURE.md, "Main thread <-> audio
// thread transport", for the tradeoff against a SharedArrayBuffer/Atomics ring buffer.

import type {
  BusId,
  EffectId,
  EffectKindInfo,
  InstrumentId,
  InstrumentKind,
  InstrumentKindInfo,
  InstrumentNoteParams,
  ModulationCapabilities,
  EffectKind,
  NodeParam,
  SampleId,
  SampleParam,
  SampleRegion,
  ScheduledEvent,
  TriggerParams,
  VoiceHandle,
  VoiceParam,
} from "./types";

export interface WireSampleData {
  sampleId: SampleId;
  name: string;
  channels: number;
  sampleRate: number;
  length: number;
  /** One ArrayBuffer per channel, planar float32, transferred (not copied). */
  channelData: ArrayBuffer[];
}

export type HostCommand =
  | { type: "load-sample"; sample: WireSampleData }
  | { type: "unload-sample"; sampleId: SampleId }
  | { type: "trigger"; voice: VoiceHandle; params: TriggerParams }
  | { type: "release"; voice: VoiceHandle }
  | { type: "stop"; voice: VoiceHandle }
  | { type: "set-voice-param"; voice: VoiceHandle; param: VoiceParam; value: number }
  | { type: "set-bus-param"; bus: BusId; param: NodeParam; value: number }
  // FX chain management (see AudioRuntime.addEffect() and ARCHITECTURE.md, "FX chains").
  | { type: "add-effect"; bus: BusId; effect: EffectId; kind: EffectKind; index: number }
  | { type: "remove-effect"; bus: BusId; effect: EffectId }
  | { type: "move-effect"; bus: BusId; effect: EffectId; index: number }
  | { type: "set-effect-enabled"; bus: BusId; effect: EffectId; enabled: boolean }
  | { type: "set-effect-param"; bus: BusId; effect: EffectId; param: NodeParam; value: number }
  // Synthesized instruments (see AudioRuntime.createInstrument() and ARCHITECTURE.md,
  // "Instruments"). Notes with a `time` in the future are scheduled sample-accurately.
  | { type: "create-instrument"; instrument: InstrumentId; kind: InstrumentKind; bus: BusId }
  | { type: "remove-instrument"; instrument: InstrumentId }
  | { type: "instrument-note-on"; instrument: InstrumentId; params: InstrumentNoteParams }
  | { type: "instrument-note-off"; instrument: InstrumentId }
  | { type: "set-instrument-param"; instrument: InstrumentId; param: number; value: number }
  // LFO modulation of an instrument parameter (AudioRuntime.setInstrumentLfo()).
  | {
      type: "set-instrument-lfo";
      instrument: InstrumentId;
      slot: number;
      enabled: boolean;
      waveform: number;
      sync: boolean;
      rateHz: number;
      division: number;
      phase: number;
      amount: number;
      target: number;
      retrigger: boolean;
    }
  // LFO modulation of an effect instance's parameter (AudioRuntime.setEffectLfo()).
  | {
      type: "set-effect-lfo";
      bus: BusId;
      effect: EffectId;
      slot: number;
      enabled: boolean;
      waveform: number;
      sync: boolean;
      rateHz: number;
      division: number;
      phase: number;
      amount: number;
      target: number;
    }
  // Timestamped parameter writes with optional linear ramps (AudioRuntime.scheduleParams()).
  | {
      type: "schedule-params";
      events: {
        time: number;
        /** 0 effect, 1 instrument, 2 sample (mirrors native ParamTarget). */
        target: number;
        bus: number;
        id: number;
        param: number;
        value: number;
        from?: number;
        ramp: number;
      }[];
    }
  | { type: "set-sample-regions"; sample: SampleId; regions: SampleRegion[] }
  | { type: "set-sample-param"; sample: SampleId; param: SampleParam; value: number }
  | { type: "schedule"; events: (ScheduledEvent & { voice: VoiceHandle })[] }
  | { type: "cancel-scheduled"; fromTime?: number }
  // Beat grid for tempo-aware instruments (AudioRuntime.setTempo()). `time` omitted = next block.
  | { type: "set-transport"; time?: number; anchorTime: number; bpm: number }
  | { type: "start-capture"; captureId: number; bus: BusId }
  | { type: "stop-capture"; captureId: number; resultSampleId: SampleId }
  // Arms a sample-accurate capture of [startTime, stopTime) (absolute engine time, same
  // domain as ScheduledEvent.time) — see AudioRuntime.armCapture() and native/src/capture.h's
  // arm(). The worklet auto-finishes it (no matching "stop" command) and posts the same
  // "capture-complete" event start-capture/stop-capture already produce.
  | { type: "arm-capture"; captureId: number; bus: BusId; startTime: number; stopTime: number; resultSampleId: SampleId }
  | { type: "configure"; maxVoices: number };

export type WorkletEvent =
  | {
      type: "ready";
      sampleRate: number;
      outputChannels: number;
      maxVoices: number;
      renderQuantumFrames: number;
      maxCaptureSeconds: number;
      instruments: InstrumentKindInfo[];
      effects: EffectKindInfo[];
      modulation: ModulationCapabilities;
    }
  | { type: "diagnostics"; activeVoices: number; loadedSamples: number; sampleMemoryBytes: number }
  // Peaks since the previous "meters" event, raw engine layout (see decodeMeters()). Posted at a
  // fixed UI-friendly rate and only while there is signal (or once as it falls silent).
  | { type: "meters"; peaks: Float32Array }
  | { type: "voice-ended"; voice: VoiceHandle }
  | {
      type: "capture-complete";
      captureId: number;
      resultSampleId: SampleId;
      channels: number;
      sampleRate: number;
      length: number;
      channelData: ArrayBuffer[];
    }
  | { type: "error"; message: string };
