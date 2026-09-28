/// <reference path="./audioWorklet.d.ts" />
import type { EngineModule } from "./engineModule.d.ts";
// @ts-expect-error -- build artifact, see engineModule.d.ts and native/build.sh
import createEngineModule from "./generated/engine.js";
import type { HostCommand, WorkletEvent } from "../runtime/commandProtocol";

// This is the entire realtime boundary: everything below runs on the browser's dedicated
// audio rendering thread, never on the main/React thread. It has no notion of pads,
// patterns, or React — only samples, voices, and the WASM engine's flat ABI. See
// ARCHITECTURE.md, "Where WASM is used" and "Threading model".
//
// Diagnostics are posted periodically (not every block) precisely so the main thread never
// becomes a dependency of the render loop: process() never waits on anything.
const DIAGNOSTICS_INTERVAL_BLOCKS = 20;

class EngineProcessor extends AudioWorkletProcessor {
  private module: EngineModule | null = null;
  private blockCounter = 0;

  constructor(options?: AudioWorkletNodeOptions) {
    super();
    const maxVoices = (options?.processorOptions?.maxVoices as number | undefined) ?? 64;
    const outputChannels = (options?.processorOptions?.outputChannels as number | undefined) ?? 2;

    this.port.onmessage = (event: MessageEvent<HostCommand>) => this.handleCommand(event.data);

    createEngineModule().then((mod: EngineModule) => {
      this.module = mod;
      mod._webdsp_init(sampleRate, outputChannels, maxVoices);
      this.postEvent({
        type: "ready",
        sampleRate,
        outputChannels: mod._webdsp_output_channels(),
        maxVoices: mod._webdsp_max_voices(),
        renderQuantumFrames: 128,
      });
    });
  }

  private postEvent(event: WorkletEvent, transfer: Transferable[] = []) {
    this.port.postMessage(event, transfer);
  }

  private timeToFrame(seconds: number): number {
    return Math.round(seconds * sampleRate);
  }

  private handleCommand(cmd: HostCommand): void {
    try {
      this.handleCommandUnsafe(cmd);
    } catch (err) {
      // An uncaught exception anywhere AudioWorkletProcessor calls into user code disables
      // the node (process() silently stops being called from then on) — for a realtime
      // engine, one malformed command must not be able to kill the entire audio graph.
      this.postEvent({ type: "error", message: `command ${cmd.type} failed: ${String(err)}` });
    }
  }

  private handleCommandUnsafe(cmd: HostCommand): void {
    const m = this.module;
    if (!m) return; // dropped if it arrives before 'ready' — see workletBridge.ts, which queues

    switch (cmd.type) {
      case "load-sample": {
        const { sampleId, channels, sampleRate: sr, length, channelData } = cmd.sample;
        const channelPtrs = channelData.map((buf) => {
          const floatData = new Float32Array(buf);
          const ptr = m._webdsp_alloc_channel_buffer(length);
          m.HEAPF32.set(floatData, ptr >> 2);
          return ptr;
        });
        const table = m._webdsp_alloc_ptr_table(channels);
        m.HEAP32.set(channelPtrs, table >> 2);
        m._webdsp_commit_sample(sampleId, channels, length, sr, table);
        break;
      }
      case "unload-sample":
        m._webdsp_remove_sample(cmd.sampleId);
        break;
      case "trigger": {
        const p = cmd.params;
        const gain = p.gain ?? 1.0;
        const rate = (p.rate ?? 1.0) * (p.pitch ? Math.pow(2, p.pitch / 12) : 1.0);
        const start = p.start ?? 0;
        const end = p.end ?? -1;
        const loop = p.loop ? 1 : 0;
        const reverse = p.reverse ? 1 : 0;
        const bus = p.bus ?? 0;
        const duration = p.duration === undefined ? -1 : this.timeToFrame(p.duration);
        if (p.time === undefined || this.timeToFrame(p.time) <= currentFrame) {
          m._webdsp_trigger(cmd.voice, p.sampleId, bus, gain, rate, start, end, loop, reverse, duration);
        } else {
          m._webdsp_schedule_event(
            this.timeToFrame(p.time), cmd.voice, p.sampleId, bus, gain, rate, start, end, loop,
            reverse, duration,
          );
        }
        break;
      }
      case "release":
        m._webdsp_release(cmd.voice);
        break;
      case "stop":
        m._webdsp_stop(cmd.voice);
        break;
      case "set-voice-param":
        m._webdsp_set_voice_param(cmd.voice, cmd.param, cmd.value);
        break;
      case "set-bus-param":
        m._webdsp_set_bus_param(cmd.bus, cmd.param, cmd.value);
        break;
      case "schedule":
        for (const e of cmd.events) {
          m._webdsp_schedule_event(
            this.timeToFrame(e.time), e.voice, e.sampleId, e.bus ?? 0, e.gain ?? 1.0,
            (e.rate ?? 1.0) * (e.pitch ? Math.pow(2, e.pitch / 12) : 1.0),
            e.start ?? 0, e.end ?? -1, e.loop ? 1 : 0, e.reverse ? 1 : 0,
            e.duration === undefined ? -1 : this.timeToFrame(e.duration),
          );
        }
        break;
      case "cancel-scheduled":
        m._webdsp_cancel_scheduled(this.timeToFrame(cmd.fromTime ?? currentTime));
        break;
      case "start-capture":
        m._webdsp_start_capture(cmd.captureId, cmd.bus);
        break;
      case "stop-capture": {
        m._webdsp_stop_capture(cmd.captureId);
        const length = m._webdsp_capture_length(cmd.captureId);
        const channels = m._webdsp_capture_channels(cmd.captureId);
        const channelData: ArrayBuffer[] = [];
        for (let c = 0; c < channels; c++) {
          const ptr = m._webdsp_capture_channel_ptr(cmd.captureId, c);
          const view = m.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + length);
          channelData.push(Float32Array.from(view).buffer);
        }
        m._webdsp_discard_capture(cmd.captureId);
        this.postEvent(
          {
            type: "capture-complete",
            captureId: cmd.captureId,
            resultSampleId: cmd.resultSampleId,
            channels,
            sampleRate,
            length,
            channelData,
          },
          channelData,
        );
        break;
      }
      case "configure":
        m._webdsp_configure_voices(cmd.maxVoices);
        break;
    }
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    try {
      return this.processUnsafe(outputs);
    } catch (err) {
      this.postEvent({ type: "error", message: `render failed: ${String(err)}` });
      return true; // keep the node alive rather than let Chrome permanently disable it
    }
  }

  private processUnsafe(outputs: Float32Array[][]): boolean {
    const m = this.module;
    const output = outputs[0];
    if (!m || !output || !output[0]) return true;

    const numFrames = output[0].length;
    m._webdsp_process(currentFrame, numFrames);

    for (let c = 0; c < output.length; c++) {
      const ptr = m._webdsp_output_channel_ptr(c);
      output[c].set(m.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + numFrames));
    }

    const endedCount = m._webdsp_ended_voice_count();
    for (let i = 0; i < endedCount; i++) {
      this.postEvent({ type: "voice-ended", voice: m._webdsp_ended_voice_id(i) });
    }

    // CPU load and underrun counts are deliberately not part of this event — see
    // ARCHITECTURE.md, "Intentionally deferred": AudioWorkletGlobalScope has no
    // performance.now() (confirmed the hard way — it isn't part of the
    // WindowOrWorkerGlobalScope mixin worklets get), so there's no in-worklet wall-clock to
    // measure render time against, and the Web Audio API exposes no per-node underrun
    // counter at all. A fabricated always-zero value would be worse than not reporting it.
    this.blockCounter++;
    if (this.blockCounter % DIAGNOSTICS_INTERVAL_BLOCKS === 0) {
      this.postEvent({
        type: "diagnostics",
        activeVoices: m._webdsp_active_voice_count(),
        loadedSamples: m._webdsp_loaded_sample_count(),
        sampleMemoryBytes: m._webdsp_sample_memory_bytes(),
      });
    }

    return true;
  }
}

registerProcessor("webdsp-engine", EngineProcessor);
