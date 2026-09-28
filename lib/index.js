var __defProp = Object.defineProperty;
var __defNormalProp = (obj, key, value) => key in obj ? __defProp(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
var __publicField = (obj, key, value) => __defNormalProp(obj, typeof key !== "symbol" ? key + "" : key, value);

// src/runtime/decode.ts
async function decodeAudioFile(data, engineSampleRate) {
  const probe = new OfflineAudioContext(1, 1, engineSampleRate);
  const buffer = await probe.decodeAudioData(data);
  const channelData = [];
  for (let ch = 0; ch < buffer.numberOfChannels; ch++) {
    channelData.push(Float32Array.from(buffer.getChannelData(ch)));
  }
  return {
    channelData,
    channels: buffer.numberOfChannels,
    sampleRate: buffer.sampleRate,
    length: buffer.length
  };
}

// src/runtime/workletBridge.ts
var WorkletBridge = class _WorkletBridge {
  constructor(node) {
    __publicField(this, "node");
    __publicField(this, "ready");
    __publicField(this, "queued", []);
    __publicField(this, "isReady", false);
    __publicField(this, "listeners", /* @__PURE__ */ new Set());
    this.node = node;
    this.ready = new Promise((resolve) => {
      this.node.port.onmessage = (ev) => {
        const data = ev.data;
        if (!this.isReady && data.type === "ready") {
          this.isReady = true;
          resolve(data);
          for (const { cmd, transfer } of this.queued.splice(0)) {
            this.node.port.postMessage(cmd, transfer);
          }
        }
        for (const listener of this.listeners) listener(data);
      };
    });
  }
  static async create(audioContext, workletModuleUrl, maxVoices, outputChannels) {
    await audioContext.audioWorklet.addModule(workletModuleUrl);
    const node = new AudioWorkletNode(audioContext, "webdsp-engine", {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [outputChannels],
      processorOptions: { maxVoices, outputChannels }
    });
    node.connect(audioContext.destination);
    return new _WorkletBridge(node);
  }
  send(cmd, transfer = []) {
    if (this.isReady) {
      this.node.port.postMessage(cmd, transfer);
    } else {
      this.queued.push({ cmd, transfer });
    }
  }
  onEvent(fn) {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }
  disconnect() {
    this.node.disconnect();
  }
};

// src/runtime/types.ts
var MASTER_BUS = 0;
var MAX_TRACK_BUSES = 32;
var VoiceParam = /* @__PURE__ */ ((VoiceParam2) => {
  VoiceParam2[VoiceParam2["Gain"] = 0] = "Gain";
  VoiceParam2[VoiceParam2["Rate"] = 1] = "Rate";
  VoiceParam2[VoiceParam2["FilterCutoff"] = 2] = "FilterCutoff";
  VoiceParam2[VoiceParam2["FilterResonance"] = 3] = "FilterResonance";
  VoiceParam2[VoiceParam2["FilterMode"] = 4] = "FilterMode";
  return VoiceParam2;
})(VoiceParam || {});
var NodeParam = /* @__PURE__ */ ((NodeParam2) => {
  NodeParam2[NodeParam2["DelayTime"] = 0] = "DelayTime";
  NodeParam2[NodeParam2["DelayFeedback"] = 1] = "DelayFeedback";
  NodeParam2[NodeParam2["DelayMix"] = 2] = "DelayMix";
  NodeParam2[NodeParam2["BusGain"] = 3] = "BusGain";
  NodeParam2[NodeParam2["FilterCutoff"] = 4] = "FilterCutoff";
  NodeParam2[NodeParam2["FilterResonance"] = 5] = "FilterResonance";
  NodeParam2[NodeParam2["FilterMode"] = 6] = "FilterMode";
  return NodeParam2;
})(NodeParam || {});
var FilterMode = /* @__PURE__ */ ((FilterMode2) => {
  FilterMode2[FilterMode2["LowPass"] = 0] = "LowPass";
  FilterMode2[FilterMode2["HighPass"] = 1] = "HighPass";
  return FilterMode2;
})(FilterMode || {});

// src/runtime/index.ts
var DEFAULT_MAX_VOICES = 64;
var AudioRuntime = class _AudioRuntime {
  constructor(ctx, bridge, capabilities) {
    __publicField(this, "ctx");
    __publicField(this, "bridge");
    __publicField(this, "capabilities");
    __publicField(this, "nextSampleId", 1);
    __publicField(this, "nextVoiceId", 1);
    __publicField(this, "nextCaptureId", 1);
    __publicField(this, "nextTrackBus", 1);
    // 0 is MASTER_BUS
    __publicField(this, "samples", /* @__PURE__ */ new Map());
    __publicField(this, "diagnostics", {
      activeVoices: 0,
      loadedSamples: 0,
      sampleMemoryBytes: 0
    });
    __publicField(this, "diagnosticsListeners", /* @__PURE__ */ new Set());
    __publicField(this, "voiceEndedListeners", /* @__PURE__ */ new Set());
    __publicField(this, "pendingCaptures", /* @__PURE__ */ new Map());
    this.ctx = ctx;
    this.bridge = bridge;
    this.capabilities = capabilities;
    this.bridge.onEvent((event) => this.handleWorkletEvent(event));
  }
  static async create(options) {
    const ctx = options.audioContext ?? new AudioContext();
    const maxVoices = options.maxVoices ?? DEFAULT_MAX_VOICES;
    if (!options.workletModuleUrl) {
      throw new Error(
        "AudioRuntime.create() requires workletModuleUrl: a fetchable URL to the built engine-processor.js. Import `defaultWorkletUrl` from 'webdsp/worklet-url' for the common case, or supply your own static asset URL."
      );
    }
    const workletUrl = options.workletModuleUrl;
    const bridge = await WorkletBridge.create(ctx, workletUrl.toString(), maxVoices, 2);
    const ready = await bridge.ready;
    const capabilities = {
      sampleRate: ready.sampleRate,
      outputChannels: ready.outputChannels,
      maxVoices: ready.maxVoices,
      renderQuantumFrames: ready.renderQuantumFrames
    };
    return new _AudioRuntime(ctx, bridge, capabilities);
  }
  // --- lifecycle ---
  async resume() {
    await this.ctx.resume();
  }
  async suspend() {
    await this.ctx.suspend();
  }
  async close() {
    this.bridge.disconnect();
    await this.ctx.close();
  }
  getCurrentTime() {
    return this.ctx.currentTime;
  }
  getCapabilities() {
    return this.capabilities;
  }
  getDiagnostics() {
    return this.diagnostics;
  }
  onDiagnostics(fn) {
    this.diagnosticsListeners.add(fn);
    return () => this.diagnosticsListeners.delete(fn);
  }
  onVoiceEnded(fn) {
    this.voiceEndedListeners.add(fn);
    return () => this.voiceEndedListeners.delete(fn);
  }
  // --- samples ---
  async loadSample(data, opts = {}) {
    const decoded = await decodeAudioFile(data, this.capabilities.sampleRate);
    const id = this.nextSampleId++;
    const meta = {
      id,
      name: opts.name ?? `sample-${id}`,
      channels: decoded.channels,
      sampleRate: decoded.sampleRate,
      length: decoded.length,
      duration: decoded.length / decoded.sampleRate,
      byteLength: decoded.length * decoded.channels * 4
    };
    const channelData = decoded.channelData.map((c) => c.buffer);
    this.bridge.send(
      {
        type: "load-sample",
        sample: { sampleId: id, name: meta.name, channels: meta.channels, sampleRate: meta.sampleRate, length: meta.length, channelData }
      },
      channelData
    );
    this.samples.set(id, meta);
    return meta;
  }
  removeSample(id) {
    this.samples.delete(id);
    this.bridge.send({ type: "unload-sample", sampleId: id });
  }
  getSample(id) {
    return this.samples.get(id);
  }
  listSamples() {
    return Array.from(this.samples.values());
  }
  // --- voices ---
  /** Triggers a sample. Returns a handle immediately (see ARCHITECTURE.md, "Voice
   * handles") even though the actual audio-thread command is delivered asynchronously. */
  trigger(params) {
    const voice = this.nextVoiceId++;
    this.bridge.send({ type: "trigger", voice, params });
    return voice;
  }
  release(voice) {
    this.bridge.send({ type: "release", voice });
  }
  stop(voice) {
    this.bridge.send({ type: "stop", voice });
  }
  setVoiceParameter(voice, param, value) {
    this.bridge.send({ type: "set-voice-param", voice, param, value });
  }
  setNodeParameter(bus, param, value) {
    this.bridge.send({ type: "set-bus-param", bus, param, value });
  }
  /** Allocates a new bus — an application-level channel of processing (e.g. one sequencer
   * track's own FX chain) with the same filter+delay capability as MASTER_BUS, summed into
   * MASTER_BUS before MASTER_BUS's own chain runs. Feed it by passing the returned id as
   * `bus` on trigger()/schedule(), and shape it with setNodeParameter(id, ...) exactly like
   * MASTER_BUS. No worklet round-trip: the engine pre-allocates MAX_TRACK_BUSES track buses
   * at init — each a no-op pass-through until parameterized — so this is a synchronous,
   * real-time-safe counter bump, not a command. Throws once MAX_TRACK_BUSES are handed out. */
  createBus() {
    if (this.nextTrackBus > MAX_TRACK_BUSES) {
      throw new Error(`createBus(): exceeded MAX_TRACK_BUSES (${MAX_TRACK_BUSES})`);
    }
    return this.nextTrackBus++;
  }
  // --- scheduling ---
  /** Compiles a batch of generic scheduled events (as an external sequencer would produce —
   * see src/sequencing for an example) into voice triggers at the given absolute engine
   * times. Returns one handle per event, in order, for later release()/stop() calls. */
  schedule(events) {
    const withVoices = events.map((e) => ({ ...e, voice: this.nextVoiceId++ }));
    this.bridge.send({ type: "schedule", events: withVoices });
    return withVoices.map((e) => e.voice);
  }
  cancelScheduled(fromTime) {
    this.bridge.send({ type: "cancel-scheduled", fromTime });
  }
  // --- resample / capture ---
  startCapture(bus = MASTER_BUS) {
    const id = this.nextCaptureId++;
    this.bridge.send({ type: "start-capture", captureId: id, bus });
    return { id };
  }
  /** Stops a capture and resolves once the recorded audio has been registered as a new
   * runtime Sample, ready to trigger() like any other. */
  stopCapture(handle) {
    const resultSampleId = this.nextSampleId++;
    return new Promise((resolve) => {
      this.pendingCaptures.set(handle.id, { resolve, sampleId: resultSampleId });
      this.bridge.send({ type: "stop-capture", captureId: handle.id, resultSampleId });
    });
  }
  // --- internal ---
  handleWorkletEvent(event) {
    switch (event.type) {
      case "diagnostics":
        this.diagnostics = {
          activeVoices: event.activeVoices,
          loadedSamples: event.loadedSamples,
          sampleMemoryBytes: event.sampleMemoryBytes
        };
        for (const l of this.diagnosticsListeners) l(this.diagnostics);
        break;
      case "voice-ended":
        for (const l of this.voiceEndedListeners) l(event.voice);
        break;
      case "capture-complete": {
        const pending = this.pendingCaptures.get(event.captureId);
        this.pendingCaptures.delete(event.captureId);
        const meta = {
          id: event.resultSampleId,
          name: `capture-${event.captureId}`,
          channels: event.channels,
          sampleRate: event.sampleRate,
          length: event.length,
          duration: event.length / event.sampleRate,
          byteLength: event.length * event.channels * 4
        };
        this.samples.set(meta.id, meta);
        this.bridge.send(
          {
            type: "load-sample",
            sample: {
              sampleId: meta.id,
              name: meta.name,
              channels: meta.channels,
              sampleRate: meta.sampleRate,
              length: meta.length,
              channelData: event.channelData
            }
          },
          event.channelData
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
};
export {
  AudioRuntime,
  FilterMode,
  MASTER_BUS,
  MAX_TRACK_BUSES,
  NodeParam,
  VoiceParam
};
//# sourceMappingURL=index.js.map