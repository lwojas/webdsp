import { hostProtocolWarning } from "./commandProtocol";
import type { HostCommand, WorkletEvent } from "./commandProtocol";
import { decodeAudioFile } from "./decode";
import { WorkletBridge } from "./workletBridge";
import { SampleParam } from "./types";
import type { EnvelopeParams } from "./types";
import type {
  BusId,
  CaptureHandle,
  EffectId,
  EffectInfo,
  EffectKind,
  EffectLfo,
  EffectParamInfo,
  InstrumentId,
  InstrumentKind,
  InstrumentLfo,
  InstrumentNoteParams,
  InstrumentParamInfo,
  ModulationCapabilities,
  RuntimeCapabilities,
  RuntimeDiagnostics,
  SampleId,
  SampleMetadata,
  SampleRegion,
  ScheduledEvent,
  ScheduledParam,
  TriggerParams,
  VoiceHandle,
  VoiceParam,
} from "./types";
import type { HostTransport } from "./hostTransport";
import { decodeMeters, emptyMeters } from "./meters";
import type { MeterSnapshot } from "./meters";
import { LfoWaveform, MASTER_BUS, NodeParam, MAX_EFFECTS_PER_BUS, MAX_INSTRUMENTS, MAX_TRACK_BUSES } from "./types";

export * from "./types";
export * from "./chop";
export { LFO_DIVISIONS, lfoShape, lfoValueAt } from "./lfo";
export * from "./meters";
export * from "./hostTransport";
export * from "./hostState";
export * from "./hostFiles";
export { HOST_PROTOCOL_VERSION, hostProtocolWarning } from "./commandProtocol";
export type { LfoEvalContext, LfoEvalSettings } from "./lfo";

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

const DEFAULT_MAX_VOICES = 128;

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
  private nextTrackBus = 1; // 0 is MASTER_BUS
  private nextEffectId = 1;
  private nextInstrumentId = 1;
  // Live instrument instances (id -> kind), for validating commands before they cross the boundary.
  private readonly instruments = new Map<InstrumentId, InstrumentKind>();
  // Main-thread mirror of every bus's FX chain, for getEffectChain() and for validating
  // commands before they cross the worklet boundary. Only addEffect/removeEffect/moveEffect/
  // setEffectEnabled mutate it, applying the same rules as native/src/fx_chain.h.
  private readonly effectChains = new Map<BusId, EffectInfo[]>();

  private readonly samples = new Map<SampleId, SampleMetadata>();
  private diagnostics: RuntimeDiagnostics = {
    activeVoices: 0,
    loadedSamples: 0,
    sampleMemoryBytes: 0,
  };
  private meters: MeterSnapshot = emptyMeters(MAX_TRACK_BUSES);
  private readonly meterListeners = new Set<(m: MeterSnapshot) => void>();
  private readonly diagnosticsListeners = new Set<(d: RuntimeDiagnostics) => void>();
  private readonly voiceEndedListeners = new Set<(v: VoiceHandle) => void>();
  private hostTransport: HostTransport | null = null;
  private readonly hostTransportListeners = new Set<(t: HostTransport) => void>();
  private readonly pendingCaptures = new Map<
    number,
    { resolve: (m: SampleMetadata) => void; sampleId: SampleId }
  >();
  // Kept separate from pendingCaptures so stopCapture()'s existing Promise<SampleMetadata>
  // contract is untouched — armCapture()'s callers additionally get the raw captured PCM back
  // (see armCapture()'s doc comment for why).
  private readonly pendingArmedCaptures = new Map<
    number,
    { resolve: (r: { metadata: SampleMetadata; channelData: ArrayBuffer[] }) => void; sampleId: SampleId }
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

    const warning = hostProtocolWarning(ready.protocolVersion);
    if (warning) console.warn(warning);
    const capabilities: RuntimeCapabilities = {
      ...(ready.protocolVersion !== undefined ? { hostProtocolVersion: ready.protocolVersion } : {}),
      sampleRate: ready.sampleRate,
      outputChannels: ready.outputChannels,
      maxVoices: ready.maxVoices,
      renderQuantumFrames: ready.renderQuantumFrames,
      maxCaptureSeconds: ready.maxCaptureSeconds,
      instruments: ready.instruments,
      effects: ready.effects,
      modulation: ready.modulation,
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

  /** Latest peak snapshot (see MeterSnapshot); all zero until the first "meters" event. */
  getMeters(): MeterSnapshot {
    return this.meters;
  }

  /** Subscribes to peak snapshots, posted ~47 Hz while there is signal and silent otherwise.
   * Each snapshot covers the interval since the previous one, so no peak is missed. Pair with
   * `MeterTracker` for hold/decay display state. */
  onMeters(fn: (m: MeterSnapshot) => void): () => void {
    this.meterListeners.add(fn);
    return () => this.meterListeners.delete(fn);
  }

  /** Configures the master limiter (a thin wrapper over setNodeParameter on MASTER_BUS). The
   * limiter is on by default with a -0.3 dBFS ceiling and 100 ms release. */
  setMasterLimiter(settings: { enabled?: boolean; ceilingDb?: number; releaseMs?: number }): void {
    if (settings.enabled !== undefined) this.setNodeParameter(MASTER_BUS, NodeParam.LimiterEnable, settings.enabled ? 1 : 0);
    if (settings.ceilingDb !== undefined) this.setNodeParameter(MASTER_BUS, NodeParam.LimiterCeiling, settings.ceilingDb);
    if (settings.releaseMs !== undefined) this.setNodeParameter(MASTER_BUS, NodeParam.LimiterRelease, settings.releaseMs);
  }

  /** The host's own transport, or null when the runtime has none (a plain browser). See hostTransport.ts. */
  getHostTransport(): HostTransport | null {
    return this.hostTransport;
  }

  /** Subscribes to host transport updates: every start, stop, locate, loop change and tempo change, plus a
   * periodic refresh while playing. Never fires in a browser. Returns an unsubscribe function. */
  onHostTransport(fn: (transport: HostTransport) => void): () => void {
    this.hostTransportListeners.add(fn);
    return () => this.hostTransportListeners.delete(fn);
  }

  onVoiceEnded(fn: (voice: VoiceHandle) => void): () => void {
    this.voiceEndedListeners.add(fn);
    return () => this.voiceEndedListeners.delete(fn);
  }

  // --- samples ---

  async loadSample(data: ArrayBuffer, opts: { name?: string; gain?: number; pan?: number; envelope?: EnvelopeParams; regions?: SampleRegion[] } = {}): Promise<SampleMetadata> {
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
    // Commands are processed in order, so this always lands after the commit above even
    // though it's a separate message — a convenience over always having to follow loadSample()
    // with a setSampleParameter() call for the common case of a fixed per-instrument gain
    // known at load time. Omitted (not sent) when the caller didn't ask for it, matching
    // SampleStore's own unity default (see native/src/sample_store.h) rather than sending a
    // redundant "set it to 1.0" on every load.
    if (opts.gain !== undefined) {
      this.bridge.send({ type: "set-sample-param", sample: id, param: SampleParam.Gain, value: opts.gain });
    }
    if (opts.pan !== undefined) {
      this.bridge.send({ type: "set-sample-param", sample: id, param: SampleParam.Pan, value: opts.pan });
    }
    if (opts.envelope) this.setSampleEnvelope(id, opts.envelope);
    this.samples.set(id, meta);
    if (opts.regions) this.setSampleRegions(id, opts.regions);
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

  /** Sets a persistent parameter on the sample itself (SampleParam.Gain, SampleParam.Pan) — not on any
   * one playing voice. Takes effect immediately for every voice currently playing that
   * sample, not just ones triggered afterward (see native/src/voice.h#render). A no-op if
   * `sample` doesn't currently reference a loaded sample. */
  setSampleParameter(sample: SampleId, param: SampleParam, value: number): void {
    this.bridge.send({ type: "set-sample-param", sample, param, value });
  }

  /** Replaces the sample's cue regions (frame offsets). Triggers with `region: i` then play
   * region `i` — see TriggerParams.region. Persistent like gain; a no-op for an unknown id. */
  setSampleRegions(sample: SampleId, regions: SampleRegion[]): void {
    const meta = this.samples.get(sample);
    if (meta) meta.regions = regions.map((r) => ({ start: r.start, end: r.end }));
    this.bridge.send({ type: "set-sample-regions", sample, regions });
  }

  /** Sets the amplitude ADSR envelope (see EnvelopeParams for units, ranges and behaviour) of
   * every voice that plays `sample`. Only the fields present are changed. A no-op if `sample`
   * doesn't reference a loaded sample. */
  setSampleEnvelope(sample: SampleId, envelope: EnvelopeParams): void {
    const send = (param: SampleParam, value: number | undefined) => {
      if (value !== undefined) this.bridge.send({ type: "set-sample-param", sample, param, value });
    };
    send(SampleParam.EnvAttack, envelope.attack);
    send(SampleParam.EnvDecay, envelope.decay);
    send(SampleParam.EnvSustain, envelope.sustain);
    send(SampleParam.EnvRelease, envelope.release);
  }

  setNodeParameter(bus: BusId, param: NodeParam, value: number): void {
    this.bridge.send({ type: "set-bus-param", bus, param, value });
  }

  // --- FX chains ---
  // A bus's FX chain is an ordered list of independent effect instances; the same kind may
  // appear any number of times. The same calls work on MASTER_BUS and on any createBus() bus.
  // Effects process in chain order. See ARCHITECTURE.md, "FX chains".

  /** Adds an instance of `kind` to `bus`'s chain at `index` (default: the end) and returns its
   * id. The new instance has its own state and parameters, and is inert until parameterized
   * with setEffectParameter(). `enabled: false` adds it bypassed. Throws if the chain already
   * holds MAX_EFFECTS_PER_BUS instances. */
  addEffect(bus: BusId, kind: EffectKind, opts: { index?: number; enabled?: boolean } = {}): EffectId {
    const chain = this.chainFor(bus);
    if (chain.length >= MAX_EFFECTS_PER_BUS) {
      throw new Error(`addEffect(): bus ${bus} already has MAX_EFFECTS_PER_BUS (${MAX_EFFECTS_PER_BUS}) effects`);
    }
    const id = this.nextEffectId++;
    const index = opts.index === undefined ? chain.length : Math.max(0, Math.min(opts.index, chain.length));
    const enabled = opts.enabled ?? true;
    chain.splice(index, 0, { id, kind, enabled });
    this.bridge.send({ type: "add-effect", bus, effect: id, kind, index });
    if (!enabled) this.bridge.send({ type: "set-effect-enabled", bus, effect: id, enabled: false });
    return id;
  }

  /** Removes the instance and frees its state. Other instances — including others of the same
   * kind — are untouched. A no-op for an unknown id. */
  removeEffect(bus: BusId, effect: EffectId): void {
    const chain = this.chainFor(bus);
    const i = chain.findIndex((e) => e.id === effect);
    if (i < 0) return;
    chain.splice(i, 1);
    this.bridge.send({ type: "remove-effect", bus, effect });
  }

  /** Moves the instance to `index` (clamped), keeping its state. */
  moveEffect(bus: BusId, effect: EffectId, index: number): void {
    const chain = this.chainFor(bus);
    const i = chain.findIndex((e) => e.id === effect);
    if (i < 0) return;
    const [info] = chain.splice(i, 1);
    const to = Math.max(0, Math.min(index, chain.length));
    chain.splice(to, 0, info);
    this.bridge.send({ type: "move-effect", bus, effect, index: to });
  }

  /** Bypasses (false) or re-enables (true) one instance. Bypass clears its internal state. */
  setEffectEnabled(bus: BusId, effect: EffectId, enabled: boolean): void {
    const info = this.chainFor(bus).find((e) => e.id === effect);
    if (!info) return;
    info.enabled = enabled;
    this.bridge.send({ type: "set-effect-enabled", bus, effect, enabled });
  }

  /** Sets a parameter of one instance, using the same NodeParam ids as setNodeParameter()
   * (e.g. NodeParam.FilterCutoff on a Filter instance). A parameter that doesn't belong to the
   * instance's kind is ignored. */
  setEffectParameter(bus: BusId, effect: EffectId, param: NodeParam, value: number): void {
    if (!this.chainFor(bus).some((e) => e.id === effect)) return;
    this.bridge.send({ type: "set-effect-param", bus, effect, param, value });
  }

  // --- effect parameters and modulation ---
  // Effects use the same LFO bank as synths (docs/modulation.md): the app owns the BASE value
  // (setEffectParameter, e.g. driven by automation) and the engine adds the LFO stack on top.

  /** The parameters of an effect kind, straight from the engine (ranges, defaults, units and
   * whether an LFO can target them). */
  getEffectParams(kind: EffectKind): readonly EffectParamInfo[] {
    return this.capabilities.effects.find((k) => k.kind === kind)?.params ?? [];
  }

  /** The params of `kind` an LFO can target. Build a modulation target picker from this. */
  getEffectModulationTargets(kind: EffectKind): readonly EffectParamInfo[] {
    return this.getEffectParams(kind).filter((p) => p.modulation !== "none");
  }

  /** Enables or edits LFO `slot` (0..maxLfos-1) on an effect instance. Same semantics as
   * setInstrumentLfo(): editing keeps the phase running, an invalid target leaves the slot off,
   * several LFOs may share a target (they add). */
  setEffectLfo(bus: BusId, effect: EffectId, slot: number, lfo: EffectLfo): void {
    if (!this.chainFor(bus).some((e) => e.id === effect)) return;
    const m = this.capabilities.modulation;
    if (!Number.isInteger(slot) || slot < 0 || slot >= m.maxLfos) return;
    const quarter = m.divisions.findIndex((d) => d.name === "1/4");
    this.bridge.send({
      type: "set-effect-lfo",
      bus,
      effect,
      slot,
      enabled: true,
      waveform: lfo.waveform ?? LfoWaveform.Sine,
      sync: lfo.sync ?? false,
      rateHz: lfo.rateHz ?? 1,
      division: lfo.division ?? (quarter >= 0 ? quarter : 0),
      phase: lfo.phase ?? 0,
      amount: lfo.amount,
      target: lfo.target,
    });
  }

  /** Removes LFO `slot` from an effect instance; its target goes back to the plain base value. */
  clearEffectLfo(bus: BusId, effect: EffectId, slot: number): void {
    if (!this.chainFor(bus).some((e) => e.id === effect)) return;
    if (!Number.isInteger(slot) || slot < 0 || slot >= this.capabilities.modulation.maxLfos) return;
    this.bridge.send({
      type: "set-effect-lfo", bus, effect, slot, enabled: false, waveform: 0, sync: false,
      rateHz: 1, division: 0, phase: 0, amount: 0, target: 0,
    });
  }

  /** Snapshot of the instances added through addEffect() on `bus`, in processing order. The
   * legacy setNodeParameter() API creates its own instances inside the engine; those are not
   * listed here. */
  getEffectChain(bus: BusId): readonly EffectInfo[] {
    return this.chainFor(bus).map((e) => ({ ...e }));
  }

  private chainFor(bus: BusId): EffectInfo[] {
    let chain = this.effectChains.get(bus);
    if (!chain) {
      chain = [];
      this.effectChains.set(bus, chain);
    }
    return chain;
  }

  // --- instruments ---
  // Synthesized audio sources. An instance is created once, routed to a bus like a sample voice
  // (so track/master FX apply), and played with notes. The application sees only notes and the
  // instrument's own small parameter set — never its internal DSP. See ARCHITECTURE.md,
  // "Instruments" and docs/kick-instrument.md.

  /** Parameter descriptions (id, name, min, max, default, unit) for `kind`, straight from the
   * engine — enough to build controls without knowing what they do inside. */
  getInstrumentParams(kind: InstrumentKind): readonly InstrumentParamInfo[] {
    return this.capabilities.instruments.find((k) => k.kind === kind)?.params ?? [];
  }

  /** Creates an instrument instance of `kind` feeding `opts.bus` (default MASTER_BUS) and
   * returns its id synchronously. One instance is one monophonic machine: a new note restarts
   * it. Create one per part that must overlap. Throws past MAX_INSTRUMENTS or for an unknown kind. */
  createInstrument(kind: InstrumentKind, opts: { bus?: BusId } = {}): InstrumentId {
    if (!this.capabilities.instruments.some((k) => k.kind === kind)) {
      throw new Error(`createInstrument(): unknown instrument kind ${kind}`);
    }
    if (this.instruments.size >= MAX_INSTRUMENTS) {
      throw new Error(`createInstrument(): exceeded MAX_INSTRUMENTS (${MAX_INSTRUMENTS})`);
    }
    const id = this.nextInstrumentId++;
    this.instruments.set(id, kind);
    this.bridge.send({ type: "create-instrument", instrument: id, kind, bus: opts.bus ?? MASTER_BUS });
    return id;
  }

  /** Frees the instance. Pending scheduled notes for it are dropped. No-op for an unknown id. */
  removeInstrument(instrument: InstrumentId): void {
    if (!this.instruments.delete(instrument)) return;
    this.bridge.send({ type: "remove-instrument", instrument });
  }

  /** Plays a note. With a future `time` it is scheduled sample-accurately (same clock as
   * schedule()); cancelScheduled() cancels it too. */
  instrumentNoteOn(instrument: InstrumentId, params: InstrumentNoteParams = {}): void {
    if (!this.instruments.has(instrument)) return;
    this.bridge.send({ type: "instrument-note-on", instrument, params });
  }

  // --- modulation (LFOs) ---
  // The LFO is a parameter-agnostic source; the synth owns each parameter and declares whether it
  // can be modulated. The base value you set with setInstrumentParameter() is never changed by an
  // LFO — the engine plays base + modulation. See docs/lfo-modulation.md.

  /** LFO limits and option lists (slots per instrument, waveform names, sync divisions). */
  getModulationInfo(): ModulationCapabilities {
    return this.capabilities.modulation;
  }

  /** The params of `kind` an LFO can target, straight from the synth's own metadata. Build a
   * modulation target picker from this; never hardcode target lists in an application. */
  getModulationTargets(kind: InstrumentKind): readonly InstrumentParamInfo[] {
    return this.getInstrumentParams(kind).filter((p) => p.modulation !== "none");
  }

  /** Enables or edits LFO `slot` (0..maxLfos-1) on an instrument. Editing keeps the LFO's phase
   * running (only a disabled -> enabled change restarts it). An invalid target (not a modulation
   * target of the instrument's kind) is rejected by the engine with an "error" event and leaves
   * the slot off. Free-mode LFOs are independent of the transport; synced ones follow setTempo(). */
  setInstrumentLfo(instrument: InstrumentId, slot: number, lfo: InstrumentLfo): void {
    if (!this.instruments.has(instrument)) return;
    const m = this.capabilities.modulation;
    if (!Number.isInteger(slot) || slot < 0 || slot >= m.maxLfos) return;
    const quarter = m.divisions.findIndex((d) => d.name === "1/4");
    this.bridge.send({
      type: "set-instrument-lfo",
      instrument,
      slot,
      enabled: true,
      waveform: lfo.waveform ?? LfoWaveform.Sine,
      sync: lfo.sync ?? false,
      rateHz: lfo.rateHz ?? 1,
      division: lfo.division ?? (quarter >= 0 ? quarter : 0),
      phase: lfo.phase ?? 0,
      amount: lfo.amount,
      target: lfo.target,
      retrigger: lfo.retrigger ?? false,
    });
  }

  /** Removes LFO `slot`; its target goes back to the plain base value. */
  clearInstrumentLfo(instrument: InstrumentId, slot: number): void {
    if (!this.instruments.has(instrument)) return;
    if (!Number.isInteger(slot) || slot < 0 || slot >= this.capabilities.modulation.maxLfos) return;
    this.bridge.send({
      type: "set-instrument-lfo", instrument, slot, enabled: false, waveform: 0, sync: false,
      rateHz: 1, division: 0, phase: 0, amount: 0, target: -1, retrigger: false,
    });
  }

  /** Chokes the instrument with a short release. Optional for one-shot drums. */
  instrumentNoteOff(instrument: InstrumentId): void {
    if (!this.instruments.has(instrument)) return;
    this.bridge.send({ type: "instrument-note-off", instrument });
  }

  /** Sets one of the instrument's own controls (ids from its kind's enum, e.g. KickParam).
   * Values are clamped by the engine to the range in getInstrumentParams(). */
  setInstrumentParameter(instrument: InstrumentId, param: number, value: number): void {
    if (!this.instruments.has(instrument)) return;
    this.bridge.send({ type: "set-instrument-param", instrument, param, value });
  }

  /** Allocates a new bus — an application-level channel of processing (e.g. one sequencer
   * track's own FX chain) with the same filter+delay capability as MASTER_BUS, summed into
   * MASTER_BUS before MASTER_BUS's own chain runs. Feed it by passing the returned id as
   * `bus` on trigger()/schedule(), and shape it with setNodeParameter(id, ...) exactly like
   * MASTER_BUS. No worklet round-trip: the engine pre-allocates MAX_TRACK_BUSES track buses
   * at init — each a no-op pass-through until parameterized — so this is a synchronous,
   * real-time-safe counter bump, not a command. Throws once MAX_TRACK_BUSES are handed out. */
  createBus(): BusId {
    if (this.nextTrackBus > MAX_TRACK_BUSES) {
      throw new Error(`createBus(): exceeded MAX_TRACK_BUSES (${MAX_TRACK_BUSES})`);
    }
    return this.nextTrackBus++;
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

  /** Sets the beat grid tempo-aware instruments (Pulse) lock their rhythm to. Beats lie at
   * `anchorTime + n * 60/bpm` seconds (same time domain as getCurrentTime()); `anchorTime`
   * defaults to `time`, or now. `time` is when the change takes effect — pass the time of the
   * step where a tempo change lands so it arrives with the notes scheduled for it (omitted =
   * immediately). For a continuous pulse across a tempo change, anchor at a beat boundary.
   * Not cancelled by cancelScheduled(). Until called the grid is 120 BPM anchored at 0. */
  setTempo(bpm: number, opts: { time?: number; anchorTime?: number } = {}): void {
    if (!Number.isFinite(bpm) || bpm <= 0) return;
    const anchorTime = opts.anchorTime ?? opts.time ?? this.getCurrentTime();
    this.bridge.send({ type: "set-transport", time: opts.time, anchorTime, bpm });
  }

  /** Schedules parameter writes on the audio clock: a step at `time`, or a linear ramp reaching
   * `value` at `time + ramp`. They write the BASE value (engine LFOs ride on top), apply at the
   * start of the 128-frame block containing `time` (about 2.7 ms), before that block's notes, and
   * are dropped by cancelScheduled() like notes (a ramp already running keeps going until a direct
   * setter, or its end). A direct setEffectParameter / setInstrumentParameter / setSampleParameter
   * on the same parameter cancels its ramp. Events for unknown targets or with non-finite numbers
   * are ignored. See docs/scheduled-params.md. */
  scheduleParams(events: readonly ScheduledParam[]): void {
    const wire: Extract<HostCommand, { type: "schedule-params" }>["events"] = [];
    for (const e of events) {
      const ramp = e.ramp ?? 0;
      if (!Number.isFinite(e.time) || !Number.isFinite(e.value) || !Number.isFinite(ramp) || ramp < 0) continue;
      if (e.from !== undefined && !Number.isFinite(e.from)) continue;
      const t = e.target;
      let target: number;
      let bus = 0;
      let id: number;
      if ("effect" in t) {
        if (!this.chainFor(t.effect.bus).some((x) => x.id === t.effect.effect)) continue;
        target = 0;
        bus = t.effect.bus;
        id = t.effect.effect;
      } else if ("instrument" in t) {
        if (!this.instruments.has(t.instrument)) continue;
        target = 1;
        id = t.instrument;
      } else {
        if (!this.samples.has(t.sample)) continue;
        target = 2;
        id = t.sample;
      }
      wire.push({ time: e.time, target, bus, id, param: e.param, value: e.value, from: e.from, ramp });
    }
    if (wire.length > 0) this.bridge.send({ type: "schedule-params", events: wire });
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
  armCapture(opts: { startTime: number; stopTime: number; bus?: BusId }): {
    handle: CaptureHandle;
    result: Promise<{ metadata: SampleMetadata; channelData: ArrayBuffer[] }>;
  } {
    const duration = opts.stopTime - opts.startTime;
    if (duration <= 0 || duration > this.capabilities.maxCaptureSeconds) {
      throw new Error(
        `armCapture(): requested duration ${duration.toFixed(2)}s is outside (0, ` +
          `${this.capabilities.maxCaptureSeconds}s] (RuntimeCapabilities.maxCaptureSeconds)`,
      );
    }
    const id = this.nextCaptureId++;
    const resultSampleId = this.nextSampleId++;
    const result = new Promise<{ metadata: SampleMetadata; channelData: ArrayBuffer[] }>((resolve) => {
      this.pendingArmedCaptures.set(id, { resolve, sampleId: resultSampleId });
    });
    this.bridge.send({
      type: "arm-capture",
      captureId: id,
      bus: opts.bus ?? MASTER_BUS,
      startTime: opts.startTime,
      stopTime: opts.stopTime,
      resultSampleId,
    });
    return { handle: { id }, result };
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
      case "meters":
        this.meters = decodeMeters(event.peaks);
        for (const l of this.meterListeners) l(this.meters);
        break;
      case "voice-ended":
        for (const l of this.voiceEndedListeners) l(event.voice);
        break;
      case "host-transport": {
        const { type: _type, ...transport } = event;
        this.hostTransport = transport;
        for (const l of this.hostTransportListeners) l(transport);
        break;
      }
      case "capture-complete": {
        const pending = this.pendingCaptures.get(event.captureId);
        this.pendingCaptures.delete(event.captureId);
        const pendingArmed = this.pendingArmedCaptures.get(event.captureId);
        this.pendingArmedCaptures.delete(event.captureId);
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
        // pendingArmed's caller gets its own copy of the PCM *before* the round-trip below
        // transfers (not copies) event.channelData's buffers to the worklet — postMessage
        // with a transfer list detaches those buffers on this side immediately, so handing
        // pendingArmed the original (post-transfer) buffers would resolve it with dead,
        // unreadable ArrayBuffers.
        const channelDataForCaller = pendingArmed ? event.channelData.map((buf) => buf.slice(0)) : undefined;
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
        pendingArmed?.resolve({ metadata: meta, channelData: channelDataForCaller! });
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
