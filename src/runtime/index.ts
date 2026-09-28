import type { WorkletEvent } from "./commandProtocol";
import { decodeAudioFile } from "./decode";
import { WorkletBridge } from "./workletBridge";
import type {
  BusId,
  CaptureHandle,
  NodeParam,
  RuntimeCapabilities,
  RuntimeDiagnostics,
  SampleId,
  SampleMetadata,
  ScheduledEvent,
  TriggerParams,
  VoiceHandle,
  VoiceParam,
} from "./types";
import { MASTER_BUS } from "./types";

export * from "./types";

export interface AudioRuntimeOptions {
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

const DEFAULT_MAX_VOICES = 64;

/**
 * The stable public boundary between an application/UI and the audio runtime. Everything
 * on this class deals in samples, voices, buses, and scheduled events — never pads,
 * patterns, or any other application concept. See ARCHITECTURE.md for the full boundary
 * rationale and what's intentionally left out of this surface for v1.
 */
export class AudioRuntime {
  private readonly ctx: AudioContext;
  private readonly bridge: WorkletBridge;
  private readonly capabilities: RuntimeCapabilities;

  private nextSampleId = 1;
  private nextVoiceId = 1;
  private nextCaptureId = 1;

  private readonly samples = new Map<SampleId, SampleMetadata>();
  private diagnostics: RuntimeDiagnostics = {
    activeVoices: 0,
    loadedSamples: 0,
    sampleMemoryBytes: 0,
  };
  private readonly diagnosticsListeners = new Set<(d: RuntimeDiagnostics) => void>();
  private readonly voiceEndedListeners = new Set<(v: VoiceHandle) => void>();
  private readonly pendingCaptures = new Map<
    number,
    { resolve: (m: SampleMetadata) => void; sampleId: SampleId }
  >();

  private constructor(ctx: AudioContext, bridge: WorkletBridge, capabilities: RuntimeCapabilities) {
    this.ctx = ctx;
    this.bridge = bridge;
    this.capabilities = capabilities;
    this.bridge.onEvent((event) => this.handleWorkletEvent(event));
  }

  static async create(options: AudioRuntimeOptions): Promise<AudioRuntime> {
    const ctx = options.audioContext ?? new AudioContext();
    const maxVoices = options.maxVoices ?? DEFAULT_MAX_VOICES;

    // There is no automatic default here on purpose: audioWorklet.addModule() always needs
    // a real, fetchable URL to the built engine-processor.js, and where that's actually
    // servable from is a property of the *consuming app's* bundler/host, not something this
    // package can know in general (see ARCHITECTURE.md, "Using this as a package"). Import
    // `defaultWorkletUrl` from "webdsp/worklet-url" for the common case (a bundler that
    // understands `new URL('./x', import.meta.url)`, e.g. Vite or webpack 5+), or point
    // this at wherever you've copied lib/engine-processor.js as a static asset.
    if (!options.workletModuleUrl) {
      throw new Error(
        "AudioRuntime.create() requires workletModuleUrl: a fetchable URL to the built " +
          "engine-processor.js. Import `defaultWorkletUrl` from 'webdsp/worklet-url' for " +
          "the common case, or supply your own static asset URL.",
      );
    }
    const workletUrl = options.workletModuleUrl;

    const bridge = await WorkletBridge.create(ctx, workletUrl.toString(), maxVoices, 2);
    const ready = await bridge.ready;

    const capabilities: RuntimeCapabilities = {
      sampleRate: ready.sampleRate,
      outputChannels: ready.outputChannels,
      maxVoices: ready.maxVoices,
      renderQuantumFrames: ready.renderQuantumFrames,
    };

    return new AudioRuntime(ctx, bridge, capabilities);
  }

  // --- lifecycle ---

  async resume(): Promise<void> {
    await this.ctx.resume();
  }

  async suspend(): Promise<void> {
    await this.ctx.suspend();
  }

  async close(): Promise<void> {
    this.bridge.disconnect();
    await this.ctx.close();
  }

  getCurrentTime(): number {
    return this.ctx.currentTime;
  }

  getCapabilities(): RuntimeCapabilities {
    return this.capabilities;
  }

  getDiagnostics(): RuntimeDiagnostics {
    return this.diagnostics;
  }

  onDiagnostics(fn: (d: RuntimeDiagnostics) => void): () => void {
    this.diagnosticsListeners.add(fn);
    return () => this.diagnosticsListeners.delete(fn);
  }

  onVoiceEnded(fn: (voice: VoiceHandle) => void): () => void {
    this.voiceEndedListeners.add(fn);
    return () => this.voiceEndedListeners.delete(fn);
  }

  // --- samples ---

  async loadSample(data: ArrayBuffer, opts: { name?: string } = {}): Promise<SampleMetadata> {
    const decoded = await decodeAudioFile(data, this.capabilities.sampleRate);
    const id = this.nextSampleId++;
    const meta: SampleMetadata = {
      id,
      name: opts.name ?? `sample-${id}`,
      channels: decoded.channels,
      sampleRate: decoded.sampleRate,
      length: decoded.length,
      duration: decoded.length / decoded.sampleRate,
      byteLength: decoded.length * decoded.channels * 4,
    };
    const channelData = decoded.channelData.map((c) => c.buffer as ArrayBuffer);
    this.bridge.send(
      {
        type: "load-sample",
        sample: { sampleId: id, name: meta.name, channels: meta.channels, sampleRate: meta.sampleRate, length: meta.length, channelData },
      },
      channelData,
    );
    this.samples.set(id, meta);
    return meta;
  }

  removeSample(id: SampleId): void {
    this.samples.delete(id);
    this.bridge.send({ type: "unload-sample", sampleId: id });
  }

  getSample(id: SampleId): SampleMetadata | undefined {
    return this.samples.get(id);
  }

  listSamples(): SampleMetadata[] {
    return Array.from(this.samples.values());
  }

  // --- voices ---

  /** Triggers a sample. Returns a handle immediately (see ARCHITECTURE.md, "Voice
   * handles") even though the actual audio-thread command is delivered asynchronously. */
  trigger(params: TriggerParams): VoiceHandle {
    const voice = this.nextVoiceId++;
    this.bridge.send({ type: "trigger", voice, params });
    return voice;
  }

  release(voice: VoiceHandle): void {
    this.bridge.send({ type: "release", voice });
  }

  stop(voice: VoiceHandle): void {
    this.bridge.send({ type: "stop", voice });
  }

  setVoiceParameter(voice: VoiceHandle, param: VoiceParam, value: number): void {
    this.bridge.send({ type: "set-voice-param", voice, param, value });
  }

  setNodeParameter(bus: BusId, param: NodeParam, value: number): void {
    this.bridge.send({ type: "set-bus-param", bus, param, value });
  }

  // --- scheduling ---

  /** Compiles a batch of generic scheduled events (as an external sequencer would produce —
   * see src/sequencing for an example) into voice triggers at the given absolute engine
   * times. Returns one handle per event, in order, for later release()/stop() calls. */
  schedule(events: ScheduledEvent[]): VoiceHandle[] {
    const withVoices = events.map((e) => ({ ...e, voice: this.nextVoiceId++ }));
    this.bridge.send({ type: "schedule", events: withVoices });
    return withVoices.map((e) => e.voice);
  }

  cancelScheduled(fromTime?: number): void {
    this.bridge.send({ type: "cancel-scheduled", fromTime });
  }

  // --- resample / capture ---

  startCapture(bus: BusId = MASTER_BUS): CaptureHandle {
    const id = this.nextCaptureId++;
    this.bridge.send({ type: "start-capture", captureId: id, bus });
    return { id };
  }

  /** Stops a capture and resolves once the recorded audio has been registered as a new
   * runtime Sample, ready to trigger() like any other. */
  stopCapture(handle: CaptureHandle): Promise<SampleMetadata> {
    const resultSampleId = this.nextSampleId++;
    return new Promise((resolve) => {
      this.pendingCaptures.set(handle.id, { resolve, sampleId: resultSampleId });
      this.bridge.send({ type: "stop-capture", captureId: handle.id, resultSampleId });
    });
  }

  // --- internal ---

  private handleWorkletEvent(event: WorkletEvent): void {
    switch (event.type) {
      case "diagnostics":
        this.diagnostics = {
          activeVoices: event.activeVoices,
          loadedSamples: event.loadedSamples,
          sampleMemoryBytes: event.sampleMemoryBytes,
        };
        for (const l of this.diagnosticsListeners) l(this.diagnostics);
        break;
      case "voice-ended":
        for (const l of this.voiceEndedListeners) l(event.voice);
        break;
      case "capture-complete": {
        const pending = this.pendingCaptures.get(event.captureId);
        this.pendingCaptures.delete(event.captureId);
        const meta: SampleMetadata = {
          id: event.resultSampleId,
          name: `capture-${event.captureId}`,
          channels: event.channels,
          sampleRate: event.sampleRate,
          length: event.length,
          duration: event.length / event.sampleRate,
          byteLength: event.length * event.channels * 4,
        };
        this.samples.set(meta.id, meta);
        // Round-trip the captured PCM back to the worklet as an ordinary load-sample
        // command so it lands in the same SampleStore any other sample lives in — commands
        // are processed in order, so it's guaranteed to be committed before any trigger()
        // the caller issues after this promise resolves.
        this.bridge.send(
          {
            type: "load-sample",
            sample: {
              sampleId: meta.id,
              name: meta.name,
              channels: meta.channels,
              sampleRate: meta.sampleRate,
              length: meta.length,
              channelData: event.channelData,
            },
          },
          event.channelData,
        );
        pending?.resolve(meta);
        break;
      }
      case "error":
        console.error("[webdsp engine]", event.message);
        break;
      case "ready":
        break;
    }
  }
}
