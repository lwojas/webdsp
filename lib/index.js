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
var MAX_TRACK_BUSES = 64;
function regionForNote(note, rootNote, regionCount) {
  const index = Math.round(note) - rootNote;
  return index >= 0 && index < regionCount ? index : void 0;
}
var VoiceParam = /* @__PURE__ */ ((VoiceParam2) => {
  VoiceParam2[VoiceParam2["Gain"] = 0] = "Gain";
  VoiceParam2[VoiceParam2["Rate"] = 1] = "Rate";
  VoiceParam2[VoiceParam2["FilterCutoff"] = 2] = "FilterCutoff";
  VoiceParam2[VoiceParam2["FilterResonance"] = 3] = "FilterResonance";
  VoiceParam2[VoiceParam2["FilterMode"] = 4] = "FilterMode";
  VoiceParam2[VoiceParam2["FilterSlope"] = 5] = "FilterSlope";
  VoiceParam2[VoiceParam2["Pan"] = 6] = "Pan";
  return VoiceParam2;
})(VoiceParam || {});
var SampleParam = /* @__PURE__ */ ((SampleParam2) => {
  SampleParam2[SampleParam2["Gain"] = 0] = "Gain";
  SampleParam2[SampleParam2["EnvAttack"] = 1] = "EnvAttack";
  SampleParam2[SampleParam2["EnvDecay"] = 2] = "EnvDecay";
  SampleParam2[SampleParam2["EnvSustain"] = 3] = "EnvSustain";
  SampleParam2[SampleParam2["EnvRelease"] = 4] = "EnvRelease";
  SampleParam2[SampleParam2["Pan"] = 5] = "Pan";
  return SampleParam2;
})(SampleParam || {});
var NodeParam = /* @__PURE__ */ ((NodeParam2) => {
  NodeParam2[NodeParam2["DelayTime"] = 0] = "DelayTime";
  NodeParam2[NodeParam2["DelayFeedback"] = 1] = "DelayFeedback";
  NodeParam2[NodeParam2["DelayMix"] = 2] = "DelayMix";
  NodeParam2[NodeParam2["BusGain"] = 3] = "BusGain";
  NodeParam2[NodeParam2["FilterCutoff"] = 4] = "FilterCutoff";
  NodeParam2[NodeParam2["FilterResonance"] = 5] = "FilterResonance";
  NodeParam2[NodeParam2["FilterMode"] = 6] = "FilterMode";
  NodeParam2[NodeParam2["ReverbDecay"] = 7] = "ReverbDecay";
  NodeParam2[NodeParam2["ReverbDamping"] = 8] = "ReverbDamping";
  NodeParam2[NodeParam2["ReverbMix"] = 9] = "ReverbMix";
  NodeParam2[NodeParam2["CompThreshold"] = 10] = "CompThreshold";
  NodeParam2[NodeParam2["CompRatio"] = 11] = "CompRatio";
  NodeParam2[NodeParam2["CompAttack"] = 12] = "CompAttack";
  NodeParam2[NodeParam2["CompRelease"] = 13] = "CompRelease";
  NodeParam2[NodeParam2["CompKnee"] = 14] = "CompKnee";
  NodeParam2[NodeParam2["CompMakeup"] = 15] = "CompMakeup";
  NodeParam2[NodeParam2["SatDrive"] = 16] = "SatDrive";
  NodeParam2[NodeParam2["SatAsymmetry"] = 17] = "SatAsymmetry";
  NodeParam2[NodeParam2["SatOutputGain"] = 18] = "SatOutputGain";
  NodeParam2[NodeParam2["SatMix"] = 19] = "SatMix";
  NodeParam2[NodeParam2["ChorusFlangerRate"] = 20] = "ChorusFlangerRate";
  NodeParam2[NodeParam2["ChorusFlangerDepth"] = 21] = "ChorusFlangerDepth";
  NodeParam2[NodeParam2["ChorusFlangerDelay"] = 22] = "ChorusFlangerDelay";
  NodeParam2[NodeParam2["ChorusFlangerFeedback"] = 23] = "ChorusFlangerFeedback";
  NodeParam2[NodeParam2["ChorusFlangerStereoPhase"] = 24] = "ChorusFlangerStereoPhase";
  NodeParam2[NodeParam2["ChorusFlangerMix"] = 25] = "ChorusFlangerMix";
  NodeParam2[NodeParam2["FilterSlope"] = 26] = "FilterSlope";
  NodeParam2[NodeParam2["BusPan"] = 27] = "BusPan";
  NodeParam2[NodeParam2["PhaserRate"] = 28] = "PhaserRate";
  NodeParam2[NodeParam2["PhaserDepth"] = 29] = "PhaserDepth";
  NodeParam2[NodeParam2["PhaserCenter"] = 30] = "PhaserCenter";
  NodeParam2[NodeParam2["PhaserFeedback"] = 31] = "PhaserFeedback";
  NodeParam2[NodeParam2["PhaserStages"] = 32] = "PhaserStages";
  NodeParam2[NodeParam2["PhaserMix"] = 33] = "PhaserMix";
  NodeParam2[NodeParam2["CrushBits"] = 34] = "CrushBits";
  NodeParam2[NodeParam2["CrushDownsample"] = 35] = "CrushDownsample";
  NodeParam2[NodeParam2["CrushMix"] = 36] = "CrushMix";
  return NodeParam2;
})(NodeParam || {});
var EffectKind = /* @__PURE__ */ ((EffectKind2) => {
  EffectKind2[EffectKind2["Filter"] = 0] = "Filter";
  EffectKind2[EffectKind2["ChorusFlanger"] = 1] = "ChorusFlanger";
  EffectKind2[EffectKind2["Delay"] = 2] = "Delay";
  EffectKind2[EffectKind2["Reverb"] = 3] = "Reverb";
  EffectKind2[EffectKind2["Compressor"] = 4] = "Compressor";
  EffectKind2[EffectKind2["Saturation"] = 5] = "Saturation";
  EffectKind2[EffectKind2["Phaser"] = 6] = "Phaser";
  EffectKind2[EffectKind2["Bitcrusher"] = 7] = "Bitcrusher";
  return EffectKind2;
})(EffectKind || {});
var MAX_EFFECTS_PER_BUS = 16;
var FilterMode = /* @__PURE__ */ ((FilterMode2) => {
  FilterMode2[FilterMode2["LowPass"] = 0] = "LowPass";
  FilterMode2[FilterMode2["HighPass"] = 1] = "HighPass";
  return FilterMode2;
})(FilterMode || {});
var FilterSlope = /* @__PURE__ */ ((FilterSlope2) => {
  FilterSlope2[FilterSlope2["Db12"] = 12] = "Db12";
  FilterSlope2[FilterSlope2["Db24"] = 24] = "Db24";
  return FilterSlope2;
})(FilterSlope || {});
var InstrumentKind = /* @__PURE__ */ ((InstrumentKind2) => {
  InstrumentKind2[InstrumentKind2["Kick"] = 0] = "Kick";
  InstrumentKind2[InstrumentKind2["Snare"] = 1] = "Snare";
  InstrumentKind2[InstrumentKind2["Clap"] = 2] = "Clap";
  InstrumentKind2[InstrumentKind2["Tom"] = 3] = "Tom";
  InstrumentKind2[InstrumentKind2["HiHat"] = 4] = "HiHat";
  InstrumentKind2[InstrumentKind2["Bass"] = 5] = "Bass";
  InstrumentKind2[InstrumentKind2["Pulse"] = 6] = "Pulse";
  InstrumentKind2[InstrumentKind2["Pad"] = 7] = "Pad";
  InstrumentKind2[InstrumentKind2["Fm"] = 8] = "Fm";
  InstrumentKind2[InstrumentKind2["Hypersaw"] = 9] = "Hypersaw";
  InstrumentKind2[InstrumentKind2["PwmBeast"] = 10] = "PwmBeast";
  InstrumentKind2[InstrumentKind2["DmThump"] = 11] = "DmThump";
  InstrumentKind2[InstrumentKind2["DmMetal"] = 12] = "DmMetal";
  InstrumentKind2[InstrumentKind2["DmTone"] = 13] = "DmTone";
  InstrumentKind2[InstrumentKind2["DmCrack"] = 14] = "DmCrack";
  InstrumentKind2[InstrumentKind2["DmChaos"] = 15] = "DmChaos";
  return InstrumentKind2;
})(InstrumentKind || {});
var KickParam = /* @__PURE__ */ ((KickParam2) => {
  KickParam2[KickParam2["Tune"] = 0] = "Tune";
  KickParam2[KickParam2["PitchDrop"] = 1] = "PitchDrop";
  KickParam2[KickParam2["PitchDecay"] = 2] = "PitchDecay";
  KickParam2[KickParam2["Decay"] = 3] = "Decay";
  KickParam2[KickParam2["Punch"] = 4] = "Punch";
  KickParam2[KickParam2["Drive"] = 5] = "Drive";
  return KickParam2;
})(KickParam || {});
var SnareParam = /* @__PURE__ */ ((SnareParam2) => {
  SnareParam2[SnareParam2["Tune"] = 0] = "Tune";
  SnareParam2[SnareParam2["Decay"] = 1] = "Decay";
  SnareParam2[SnareParam2["Snap"] = 2] = "Snap";
  SnareParam2[SnareParam2["Noise"] = 3] = "Noise";
  return SnareParam2;
})(SnareParam || {});
var ClapParam = /* @__PURE__ */ ((ClapParam2) => {
  ClapParam2[ClapParam2["Tune"] = 0] = "Tune";
  ClapParam2[ClapParam2["Decay"] = 1] = "Decay";
  ClapParam2[ClapParam2["Spread"] = 2] = "Spread";
  ClapParam2[ClapParam2["Snap"] = 3] = "Snap";
  return ClapParam2;
})(ClapParam || {});
var TomParam = /* @__PURE__ */ ((TomParam2) => {
  TomParam2[TomParam2["Tune"] = 0] = "Tune";
  TomParam2[TomParam2["PitchDrop"] = 1] = "PitchDrop";
  TomParam2[TomParam2["Decay"] = 2] = "Decay";
  TomParam2[TomParam2["Body"] = 3] = "Body";
  TomParam2[TomParam2["Damp"] = 4] = "Damp";
  return TomParam2;
})(TomParam || {});
var HiHatParam = /* @__PURE__ */ ((HiHatParam2) => {
  HiHatParam2[HiHatParam2["Tune"] = 0] = "Tune";
  HiHatParam2[HiHatParam2["Decay"] = 1] = "Decay";
  HiHatParam2[HiHatParam2["Metal"] = 2] = "Metal";
  HiHatParam2[HiHatParam2["Tone"] = 3] = "Tone";
  return HiHatParam2;
})(HiHatParam || {});
var BassParam = /* @__PURE__ */ ((BassParam2) => {
  BassParam2[BassParam2["Tune"] = 0] = "Tune";
  BassParam2[BassParam2["Body"] = 1] = "Body";
  BassParam2[BassParam2["Bite"] = 2] = "Bite";
  BassParam2[BassParam2["Cutoff"] = 3] = "Cutoff";
  BassParam2[BassParam2["Resonance"] = 4] = "Resonance";
  BassParam2[BassParam2["Attack"] = 5] = "Attack";
  BassParam2[BassParam2["Decay"] = 6] = "Decay";
  BassParam2[BassParam2["Sustain"] = 7] = "Sustain";
  BassParam2[BassParam2["Release"] = 8] = "Release";
  BassParam2[BassParam2["Legato"] = 9] = "Legato";
  BassParam2[BassParam2["Slide"] = 10] = "Slide";
  return BassParam2;
})(BassParam || {});
var PulseParam = /* @__PURE__ */ ((PulseParam2) => {
  PulseParam2[PulseParam2["Tune"] = 0] = "Tune";
  PulseParam2[PulseParam2["Cutoff"] = 1] = "Cutoff";
  PulseParam2[PulseParam2["Resonance"] = 2] = "Resonance";
  PulseParam2[PulseParam2["Pump"] = 3] = "Pump";
  PulseParam2[PulseParam2["Repeat"] = 4] = "Repeat";
  PulseParam2[PulseParam2["Rate"] = 5] = "Rate";
  PulseParam2[PulseParam2["Diffuse"] = 6] = "Diffuse";
  PulseParam2[PulseParam2["Attack"] = 7] = "Attack";
  PulseParam2[PulseParam2["Release"] = 8] = "Release";
  return PulseParam2;
})(PulseParam || {});
var PwmBeastParam = /* @__PURE__ */ ((PwmBeastParam2) => {
  PwmBeastParam2[PwmBeastParam2["Tune"] = 0] = "Tune";
  PwmBeastParam2[PwmBeastParam2["Width"] = 1] = "Width";
  PwmBeastParam2[PwmBeastParam2["Pwm"] = 2] = "Pwm";
  PwmBeastParam2[PwmBeastParam2["Rate"] = 3] = "Rate";
  PwmBeastParam2[PwmBeastParam2["Shape"] = 4] = "Shape";
  PwmBeastParam2[PwmBeastParam2["Harmonics"] = 5] = "Harmonics";
  PwmBeastParam2[PwmBeastParam2["Drive"] = 6] = "Drive";
  PwmBeastParam2[PwmBeastParam2["Cutoff"] = 7] = "Cutoff";
  PwmBeastParam2[PwmBeastParam2["Resonance"] = 8] = "Resonance";
  PwmBeastParam2[PwmBeastParam2["EnvAmount"] = 9] = "EnvAmount";
  PwmBeastParam2[PwmBeastParam2["Sub"] = 10] = "Sub";
  PwmBeastParam2[PwmBeastParam2["Attack"] = 11] = "Attack";
  PwmBeastParam2[PwmBeastParam2["Decay"] = 12] = "Decay";
  PwmBeastParam2[PwmBeastParam2["Sustain"] = 13] = "Sustain";
  PwmBeastParam2[PwmBeastParam2["Release"] = 14] = "Release";
  return PwmBeastParam2;
})(PwmBeastParam || {});
var DmThumpParam = /* @__PURE__ */ ((DmThumpParam2) => {
  DmThumpParam2[DmThumpParam2["Tune"] = 0] = "Tune";
  DmThumpParam2[DmThumpParam2["Decay"] = 1] = "Decay";
  DmThumpParam2[DmThumpParam2["Fm"] = 2] = "Fm";
  DmThumpParam2[DmThumpParam2["Sweep"] = 3] = "Sweep";
  DmThumpParam2[DmThumpParam2["Punch"] = 4] = "Punch";
  DmThumpParam2[DmThumpParam2["Ratio"] = 5] = "Ratio";
  DmThumpParam2[DmThumpParam2["Body"] = 6] = "Body";
  DmThumpParam2[DmThumpParam2["Drive"] = 7] = "Drive";
  return DmThumpParam2;
})(DmThumpParam || {});
var DmMetalParam = /* @__PURE__ */ ((DmMetalParam2) => {
  DmMetalParam2[DmMetalParam2["Tune"] = 0] = "Tune";
  DmMetalParam2[DmMetalParam2["Decay"] = 1] = "Decay";
  DmMetalParam2[DmMetalParam2["Fm"] = 2] = "Fm";
  DmMetalParam2[DmMetalParam2["Spread"] = 3] = "Spread";
  DmMetalParam2[DmMetalParam2["Tone"] = 4] = "Tone";
  DmMetalParam2[DmMetalParam2["Noise"] = 5] = "Noise";
  DmMetalParam2[DmMetalParam2["Chick"] = 6] = "Chick";
  DmMetalParam2[DmMetalParam2["Drive"] = 7] = "Drive";
  return DmMetalParam2;
})(DmMetalParam || {});
var DmToneParam = /* @__PURE__ */ ((DmToneParam2) => {
  DmToneParam2[DmToneParam2["Tune"] = 0] = "Tune";
  DmToneParam2[DmToneParam2["Decay"] = 1] = "Decay";
  DmToneParam2[DmToneParam2["Fm"] = 2] = "Fm";
  DmToneParam2[DmToneParam2["Structure"] = 3] = "Structure";
  DmToneParam2[DmToneParam2["Ratio"] = 4] = "Ratio";
  DmToneParam2[DmToneParam2["Damp"] = 5] = "Damp";
  DmToneParam2[DmToneParam2["Attack"] = 6] = "Attack";
  DmToneParam2[DmToneParam2["Hold"] = 7] = "Hold";
  DmToneParam2[DmToneParam2["Drive"] = 8] = "Drive";
  return DmToneParam2;
})(DmToneParam || {});
var DmCrackParam = /* @__PURE__ */ ((DmCrackParam2) => {
  DmCrackParam2[DmCrackParam2["Tune"] = 0] = "Tune";
  DmCrackParam2[DmCrackParam2["Decay"] = 1] = "Decay";
  DmCrackParam2[DmCrackParam2["Fm"] = 2] = "Fm";
  DmCrackParam2[DmCrackParam2["Body"] = 3] = "Body";
  DmCrackParam2[DmCrackParam2["Noise"] = 4] = "Noise";
  DmCrackParam2[DmCrackParam2["Burst"] = 5] = "Burst";
  DmCrackParam2[DmCrackParam2["Tone"] = 6] = "Tone";
  DmCrackParam2[DmCrackParam2["Drive"] = 7] = "Drive";
  return DmCrackParam2;
})(DmCrackParam || {});
var DmChaosParam = /* @__PURE__ */ ((DmChaosParam2) => {
  DmChaosParam2[DmChaosParam2["Tune"] = 0] = "Tune";
  DmChaosParam2[DmChaosParam2["Decay"] = 1] = "Decay";
  DmChaosParam2[DmChaosParam2["Chaos"] = 2] = "Chaos";
  DmChaosParam2[DmChaosParam2["Ratio"] = 3] = "Ratio";
  DmChaosParam2[DmChaosParam2["Fold"] = 4] = "Fold";
  DmChaosParam2[DmChaosParam2["Noise"] = 5] = "Noise";
  DmChaosParam2[DmChaosParam2["Sweep"] = 6] = "Sweep";
  DmChaosParam2[DmChaosParam2["Hold"] = 7] = "Hold";
  return DmChaosParam2;
})(DmChaosParam || {});
var PadParam = /* @__PURE__ */ ((PadParam2) => {
  PadParam2[PadParam2["Tune"] = 0] = "Tune";
  PadParam2[PadParam2["Tone"] = 1] = "Tone";
  PadParam2[PadParam2["Movement"] = 2] = "Movement";
  PadParam2[PadParam2["Detune"] = 3] = "Detune";
  PadParam2[PadParam2["Space"] = 4] = "Space";
  PadParam2[PadParam2["Attack"] = 5] = "Attack";
  PadParam2[PadParam2["Release"] = 6] = "Release";
  return PadParam2;
})(PadParam || {});
var FmParam = /* @__PURE__ */ ((FmParam2) => {
  FmParam2[FmParam2["Tune"] = 0] = "Tune";
  FmParam2[FmParam2["Ratio"] = 1] = "Ratio";
  FmParam2[FmParam2["Timbre"] = 2] = "Timbre";
  FmParam2[FmParam2["Snap"] = 3] = "Snap";
  FmParam2[FmParam2["Tone"] = 4] = "Tone";
  FmParam2[FmParam2["Attack"] = 5] = "Attack";
  FmParam2[FmParam2["Decay"] = 6] = "Decay";
  FmParam2[FmParam2["Sustain"] = 7] = "Sustain";
  FmParam2[FmParam2["Release"] = 8] = "Release";
  FmParam2[FmParam2["Legato"] = 9] = "Legato";
  FmParam2[FmParam2["Slide"] = 10] = "Slide";
  return FmParam2;
})(FmParam || {});
var HypersawParam = /* @__PURE__ */ ((HypersawParam2) => {
  HypersawParam2[HypersawParam2["Tune"] = 0] = "Tune";
  HypersawParam2[HypersawParam2["Detune"] = 1] = "Detune";
  HypersawParam2[HypersawParam2["Spread"] = 2] = "Spread";
  HypersawParam2[HypersawParam2["Density"] = 3] = "Density";
  HypersawParam2[HypersawParam2["Cutoff"] = 4] = "Cutoff";
  HypersawParam2[HypersawParam2["Attack"] = 5] = "Attack";
  HypersawParam2[HypersawParam2["Decay"] = 6] = "Decay";
  HypersawParam2[HypersawParam2["Sustain"] = 7] = "Sustain";
  HypersawParam2[HypersawParam2["Release"] = 8] = "Release";
  return HypersawParam2;
})(HypersawParam || {});
var LfoWaveform = /* @__PURE__ */ ((LfoWaveform2) => {
  LfoWaveform2[LfoWaveform2["Sine"] = 0] = "Sine";
  LfoWaveform2[LfoWaveform2["Triangle"] = 1] = "Triangle";
  LfoWaveform2[LfoWaveform2["Square"] = 2] = "Square";
  LfoWaveform2[LfoWaveform2["Saw"] = 3] = "Saw";
  return LfoWaveform2;
})(LfoWaveform || {});
var MAX_INSTRUMENTS = 64;

// src/runtime/chop.ts
var MIN_SLICE = 2e-3;
var MAX_SLICES = 256;
function normalizeCues(cues, maxSlices = MAX_SLICES) {
  const sorted = cues.filter((c) => Number.isFinite(c)).map((c) => Math.min(1, Math.max(0, c))).sort((a, b) => a - b);
  const result = [];
  let prev = 0;
  for (const c of sorted) {
    if (c - prev < MIN_SLICE || 1 - c < MIN_SLICE) continue;
    if (result.length >= maxSlices - 1) break;
    result.push(c);
    prev = c;
  }
  return result;
}
function evenCues(slices, maxSlices = MAX_SLICES) {
  const n = Math.min(maxSlices, Math.max(1, Math.round(slices)));
  return Array.from({ length: n - 1 }, (_, i) => (i + 1) / n);
}
function sliceCount(cues) {
  return cues && cues.length > 0 ? cues.length + 1 : 0;
}
function addCue(cues, at, maxSlices = MAX_SLICES) {
  return normalizeCues([...cues, at], maxSlices);
}
function moveCue(cues, index, to) {
  if (index < 0 || index >= cues.length) return [...cues];
  const lo = (index > 0 ? cues[index - 1] : 0) + MIN_SLICE;
  const hi = (index < cues.length - 1 ? cues[index + 1] : 1) - MIN_SLICE;
  const next = [...cues];
  next[index] = Math.min(hi, Math.max(lo, to));
  return next;
}
function removeCue(cues, index) {
  return cues.filter((_, i) => i !== index);
}
function cueRegions(cues, length) {
  const bounds = [0, ...cues, 1].map((f) => Math.round(f * length));
  return bounds.slice(0, -1).map((start, i) => ({ start, end: bounds[i + 1] }));
}
function computePeaks(channels, bins = 1024) {
  const peaks = new Float32Array(bins);
  const length = channels[0]?.length ?? 0;
  if (length === 0) return peaks;
  for (const data of channels) {
    for (let b = 0; b < bins; b++) {
      const from = Math.floor(b * length / bins);
      const to = Math.max(from + 1, Math.floor((b + 1) * length / bins));
      let max = peaks[b];
      for (let i = from; i < to && i < length; i++) {
        const v = Math.abs(data[i]);
        if (v > max) max = v;
      }
      peaks[b] = max;
    }
  }
  return peaks;
}

// src/runtime/lfo.ts
var LFO_DIVISIONS = [
  { name: "4 bars", beats: 16 },
  { name: "2 bars", beats: 8 },
  { name: "1/1", beats: 4 },
  { name: "1/2.", beats: 3 },
  { name: "1/2", beats: 2 },
  { name: "1/4.", beats: 1.5 },
  { name: "1/2T", beats: 4 / 3 },
  { name: "1/4", beats: 1 },
  { name: "1/8.", beats: 0.75 },
  { name: "1/4T", beats: 2 / 3 },
  { name: "1/8", beats: 0.5 },
  { name: "1/16.", beats: 0.375 },
  { name: "1/8T", beats: 1 / 3 },
  { name: "1/16", beats: 0.25 },
  { name: "1/16T", beats: 1 / 6 },
  { name: "1/32", beats: 0.125 }
];
function lfoShape(waveform, phase) {
  const p = phase - Math.floor(phase);
  switch (waveform) {
    case 0 /* Sine */:
      return Math.sin(2 * Math.PI * p);
    case 1 /* Triangle */:
      return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
    case 2 /* Square */:
      return p < 0.5 ? 1 : -1;
    case 3 /* Saw */:
      return 2 * p - 1;
  }
  return 0;
}
function lfoValueAt(s, ctx) {
  const elapsed = ctx.time - ctx.anchorTime;
  let cycles;
  if (s.sync) {
    const division = LFO_DIVISIONS[Math.min(LFO_DIVISIONS.length - 1, Math.max(0, Math.round(s.division)))];
    cycles = elapsed / (60 / ctx.bpm * division.beats);
  } else {
    cycles = elapsed * s.rateHz;
  }
  return lfoShape(s.waveform, cycles + (s.phase ?? 0));
}

// src/runtime/index.ts
var DEFAULT_MAX_VOICES = 128;
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
    __publicField(this, "nextEffectId", 1);
    __publicField(this, "nextInstrumentId", 1);
    // Live instrument instances (id -> kind), for validating commands before they cross the boundary.
    __publicField(this, "instruments", /* @__PURE__ */ new Map());
    // Main-thread mirror of every bus's FX chain, for getEffectChain() and for validating
    // commands before they cross the worklet boundary. Only addEffect/removeEffect/moveEffect/
    // setEffectEnabled mutate it, applying the same rules as native/src/fx_chain.h.
    __publicField(this, "effectChains", /* @__PURE__ */ new Map());
    __publicField(this, "samples", /* @__PURE__ */ new Map());
    __publicField(this, "diagnostics", {
      activeVoices: 0,
      loadedSamples: 0,
      sampleMemoryBytes: 0
    });
    __publicField(this, "diagnosticsListeners", /* @__PURE__ */ new Set());
    __publicField(this, "voiceEndedListeners", /* @__PURE__ */ new Set());
    __publicField(this, "pendingCaptures", /* @__PURE__ */ new Map());
    // Kept separate from pendingCaptures so stopCapture()'s existing Promise<SampleMetadata>
    // contract is untouched — armCapture()'s callers additionally get the raw captured PCM back
    // (see armCapture()'s doc comment for why).
    __publicField(this, "pendingArmedCaptures", /* @__PURE__ */ new Map());
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
      renderQuantumFrames: ready.renderQuantumFrames,
      maxCaptureSeconds: ready.maxCaptureSeconds,
      instruments: ready.instruments,
      modulation: ready.modulation
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
    if (opts.gain !== void 0) {
      this.bridge.send({ type: "set-sample-param", sample: id, param: 0 /* Gain */, value: opts.gain });
    }
    if (opts.pan !== void 0) {
      this.bridge.send({ type: "set-sample-param", sample: id, param: 5 /* Pan */, value: opts.pan });
    }
    if (opts.envelope) this.setSampleEnvelope(id, opts.envelope);
    this.samples.set(id, meta);
    if (opts.regions) this.setSampleRegions(id, opts.regions);
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
  /** Sets a persistent parameter on the sample itself (SampleParam.Gain, SampleParam.Pan) — not on any
   * one playing voice. Takes effect immediately for every voice currently playing that
   * sample, not just ones triggered afterward (see native/src/voice.h#render). A no-op if
   * `sample` doesn't currently reference a loaded sample. */
  setSampleParameter(sample, param, value) {
    this.bridge.send({ type: "set-sample-param", sample, param, value });
  }
  /** Replaces the sample's cue regions (frame offsets). Triggers with `region: i` then play
   * region `i` — see TriggerParams.region. Persistent like gain; a no-op for an unknown id. */
  setSampleRegions(sample, regions) {
    const meta = this.samples.get(sample);
    if (meta) meta.regions = regions.map((r) => ({ start: r.start, end: r.end }));
    this.bridge.send({ type: "set-sample-regions", sample, regions });
  }
  /** Sets the amplitude ADSR envelope (see EnvelopeParams for units, ranges and behaviour) of
   * every voice that plays `sample`. Only the fields present are changed. A no-op if `sample`
   * doesn't reference a loaded sample. */
  setSampleEnvelope(sample, envelope) {
    const send = (param, value) => {
      if (value !== void 0) this.bridge.send({ type: "set-sample-param", sample, param, value });
    };
    send(1 /* EnvAttack */, envelope.attack);
    send(2 /* EnvDecay */, envelope.decay);
    send(3 /* EnvSustain */, envelope.sustain);
    send(4 /* EnvRelease */, envelope.release);
  }
  setNodeParameter(bus, param, value) {
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
  addEffect(bus, kind, opts = {}) {
    const chain = this.chainFor(bus);
    if (chain.length >= MAX_EFFECTS_PER_BUS) {
      throw new Error(`addEffect(): bus ${bus} already has MAX_EFFECTS_PER_BUS (${MAX_EFFECTS_PER_BUS}) effects`);
    }
    const id = this.nextEffectId++;
    const index = opts.index === void 0 ? chain.length : Math.max(0, Math.min(opts.index, chain.length));
    const enabled = opts.enabled ?? true;
    chain.splice(index, 0, { id, kind, enabled });
    this.bridge.send({ type: "add-effect", bus, effect: id, kind, index });
    if (!enabled) this.bridge.send({ type: "set-effect-enabled", bus, effect: id, enabled: false });
    return id;
  }
  /** Removes the instance and frees its state. Other instances — including others of the same
   * kind — are untouched. A no-op for an unknown id. */
  removeEffect(bus, effect) {
    const chain = this.chainFor(bus);
    const i = chain.findIndex((e) => e.id === effect);
    if (i < 0) return;
    chain.splice(i, 1);
    this.bridge.send({ type: "remove-effect", bus, effect });
  }
  /** Moves the instance to `index` (clamped), keeping its state. */
  moveEffect(bus, effect, index) {
    const chain = this.chainFor(bus);
    const i = chain.findIndex((e) => e.id === effect);
    if (i < 0) return;
    const [info] = chain.splice(i, 1);
    const to = Math.max(0, Math.min(index, chain.length));
    chain.splice(to, 0, info);
    this.bridge.send({ type: "move-effect", bus, effect, index: to });
  }
  /** Bypasses (false) or re-enables (true) one instance. Bypass clears its internal state. */
  setEffectEnabled(bus, effect, enabled) {
    const info = this.chainFor(bus).find((e) => e.id === effect);
    if (!info) return;
    info.enabled = enabled;
    this.bridge.send({ type: "set-effect-enabled", bus, effect, enabled });
  }
  /** Sets a parameter of one instance, using the same NodeParam ids as setNodeParameter()
   * (e.g. NodeParam.FilterCutoff on a Filter instance). A parameter that doesn't belong to the
   * instance's kind is ignored. */
  setEffectParameter(bus, effect, param, value) {
    if (!this.chainFor(bus).some((e) => e.id === effect)) return;
    this.bridge.send({ type: "set-effect-param", bus, effect, param, value });
  }
  /** Snapshot of the instances added through addEffect() on `bus`, in processing order. The
   * legacy setNodeParameter() API creates its own instances inside the engine; those are not
   * listed here. */
  getEffectChain(bus) {
    return this.chainFor(bus).map((e) => ({ ...e }));
  }
  chainFor(bus) {
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
  getInstrumentParams(kind) {
    return this.capabilities.instruments.find((k) => k.kind === kind)?.params ?? [];
  }
  /** Creates an instrument instance of `kind` feeding `opts.bus` (default MASTER_BUS) and
   * returns its id synchronously. One instance is one monophonic machine: a new note restarts
   * it. Create one per part that must overlap. Throws past MAX_INSTRUMENTS or for an unknown kind. */
  createInstrument(kind, opts = {}) {
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
  removeInstrument(instrument) {
    if (!this.instruments.delete(instrument)) return;
    this.bridge.send({ type: "remove-instrument", instrument });
  }
  /** Plays a note. With a future `time` it is scheduled sample-accurately (same clock as
   * schedule()); cancelScheduled() cancels it too. */
  instrumentNoteOn(instrument, params = {}) {
    if (!this.instruments.has(instrument)) return;
    this.bridge.send({ type: "instrument-note-on", instrument, params });
  }
  // --- modulation (LFOs) ---
  // The LFO is a parameter-agnostic source; the synth owns each parameter and declares whether it
  // can be modulated. The base value you set with setInstrumentParameter() is never changed by an
  // LFO — the engine plays base + modulation. See docs/lfo-modulation.md.
  /** LFO limits and option lists (slots per instrument, waveform names, sync divisions). */
  getModulationInfo() {
    return this.capabilities.modulation;
  }
  /** The params of `kind` an LFO can target, straight from the synth's own metadata. Build a
   * modulation target picker from this; never hardcode target lists in an application. */
  getModulationTargets(kind) {
    return this.getInstrumentParams(kind).filter((p) => p.modulation !== "none");
  }
  /** Enables or edits LFO `slot` (0..maxLfos-1) on an instrument. Editing keeps the LFO's phase
   * running (only a disabled -> enabled change restarts it). An invalid target (not a modulation
   * target of the instrument's kind) is rejected by the engine with an "error" event and leaves
   * the slot off. Free-mode LFOs are independent of the transport; synced ones follow setTempo(). */
  setInstrumentLfo(instrument, slot, lfo) {
    if (!this.instruments.has(instrument)) return;
    const m = this.capabilities.modulation;
    if (!Number.isInteger(slot) || slot < 0 || slot >= m.maxLfos) return;
    const quarter = m.divisions.findIndex((d) => d.name === "1/4");
    this.bridge.send({
      type: "set-instrument-lfo",
      instrument,
      slot,
      enabled: true,
      waveform: lfo.waveform ?? 0 /* Sine */,
      sync: lfo.sync ?? false,
      rateHz: lfo.rateHz ?? 1,
      division: lfo.division ?? (quarter >= 0 ? quarter : 0),
      phase: lfo.phase ?? 0,
      amount: lfo.amount,
      target: lfo.target,
      retrigger: lfo.retrigger ?? false
    });
  }
  /** Removes LFO `slot`; its target goes back to the plain base value. */
  clearInstrumentLfo(instrument, slot) {
    if (!this.instruments.has(instrument)) return;
    if (!Number.isInteger(slot) || slot < 0 || slot >= this.capabilities.modulation.maxLfos) return;
    this.bridge.send({
      type: "set-instrument-lfo",
      instrument,
      slot,
      enabled: false,
      waveform: 0,
      sync: false,
      rateHz: 1,
      division: 0,
      phase: 0,
      amount: 0,
      target: -1,
      retrigger: false
    });
  }
  /** Chokes the instrument with a short release. Optional for one-shot drums. */
  instrumentNoteOff(instrument) {
    if (!this.instruments.has(instrument)) return;
    this.bridge.send({ type: "instrument-note-off", instrument });
  }
  /** Sets one of the instrument's own controls (ids from its kind's enum, e.g. KickParam).
   * Values are clamped by the engine to the range in getInstrumentParams(). */
  setInstrumentParameter(instrument, param, value) {
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
  /** Sets the beat grid tempo-aware instruments (Pulse) lock their rhythm to. Beats lie at
   * `anchorTime + n * 60/bpm` seconds (same time domain as getCurrentTime()); `anchorTime`
   * defaults to `time`, or now. `time` is when the change takes effect — pass the time of the
   * step where a tempo change lands so it arrives with the notes scheduled for it (omitted =
   * immediately). For a continuous pulse across a tempo change, anchor at a beat boundary.
   * Not cancelled by cancelScheduled(). Until called the grid is 120 BPM anchored at 0. */
  setTempo(bpm, opts = {}) {
    if (!Number.isFinite(bpm) || bpm <= 0) return;
    const anchorTime = opts.anchorTime ?? opts.time ?? this.getCurrentTime();
    this.bridge.send({ type: "set-transport", time: opts.time, anchorTime, bpm });
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
  armCapture(opts) {
    const duration = opts.stopTime - opts.startTime;
    if (duration <= 0 || duration > this.capabilities.maxCaptureSeconds) {
      throw new Error(
        `armCapture(): requested duration ${duration.toFixed(2)}s is outside (0, ${this.capabilities.maxCaptureSeconds}s] (RuntimeCapabilities.maxCaptureSeconds)`
      );
    }
    const id = this.nextCaptureId++;
    const resultSampleId = this.nextSampleId++;
    const result = new Promise((resolve) => {
      this.pendingArmedCaptures.set(id, { resolve, sampleId: resultSampleId });
    });
    this.bridge.send({
      type: "arm-capture",
      captureId: id,
      bus: opts.bus ?? MASTER_BUS,
      startTime: opts.startTime,
      stopTime: opts.stopTime,
      resultSampleId
    });
    return { handle: { id }, result };
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
        const pendingArmed = this.pendingArmedCaptures.get(event.captureId);
        this.pendingArmedCaptures.delete(event.captureId);
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
        const channelDataForCaller = pendingArmed ? event.channelData.map((buf) => buf.slice(0)) : void 0;
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
        pendingArmed?.resolve({ metadata: meta, channelData: channelDataForCaller });
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
  BassParam,
  ClapParam,
  DmChaosParam,
  DmCrackParam,
  DmMetalParam,
  DmThumpParam,
  DmToneParam,
  EffectKind,
  FilterMode,
  FilterSlope,
  FmParam,
  HiHatParam,
  HypersawParam,
  InstrumentKind,
  KickParam,
  LFO_DIVISIONS,
  LfoWaveform,
  MASTER_BUS,
  MAX_EFFECTS_PER_BUS,
  MAX_INSTRUMENTS,
  MAX_SLICES,
  MAX_TRACK_BUSES,
  MIN_SLICE,
  NodeParam,
  PadParam,
  PulseParam,
  PwmBeastParam,
  SampleParam,
  SnareParam,
  TomParam,
  VoiceParam,
  addCue,
  computePeaks,
  cueRegions,
  evenCues,
  lfoShape,
  lfoValueAt,
  moveCue,
  normalizeCues,
  regionForNote,
  removeCue,
  sliceCount
};
//# sourceMappingURL=index.js.map