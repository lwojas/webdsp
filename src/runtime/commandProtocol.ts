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
  NodeParam,
  SampleId,
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
  | { type: "schedule"; events: (ScheduledEvent & { voice: VoiceHandle })[] }
  | { type: "cancel-scheduled"; fromTime?: number }
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
    }
  | { type: "diagnostics"; activeVoices: number; loadedSamples: number; sampleMemoryBytes: number }
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
