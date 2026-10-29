var __defProp = Object.defineProperty;
var __defNormalProp = (obj, key, value) => key in obj ? __defProp(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
var __publicField = (obj, key, value) => __defNormalProp(obj, typeof key !== "symbol" ? key + "" : key, value);

// src/runtime/commandProtocol.ts
var HOST_PROTOCOL_VERSION = 4;
function hostProtocolWarning(hostVersion) {
  if (hostVersion === void 0 || hostVersion >= HOST_PROTOCOL_VERSION) return null;
  return `webdsp: the host implements protocol ${hostVersion} but this app expects ${HOST_PROTOCOL_VERSION}. Some features may not work until the host (plugin) is updated.`;
}

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
  NodeParam2[NodeParam2["BusOutput"] = 37] = "BusOutput";
  NodeParam2[NodeParam2["LimiterEnable"] = 38] = "LimiterEnable";
  NodeParam2[NodeParam2["LimiterCeiling"] = 39] = "LimiterCeiling";
  NodeParam2[NodeParam2["LimiterRelease"] = 40] = "LimiterRelease";
  NodeParam2[NodeParam2["UtilityGain"] = 41] = "UtilityGain";
  NodeParam2[NodeParam2["BusSendTarget"] = 42] = "BusSendTarget";
  NodeParam2[NodeParam2["BusSendAmount"] = 43] = "BusSendAmount";
  NodeParam2[NodeParam2["BusMainGain"] = 44] = "BusMainGain";
  NodeParam2[NodeParam2["DubDelayDivision"] = 45] = "DubDelayDivision";
  NodeParam2[NodeParam2["DubDelayFeedback"] = 46] = "DubDelayFeedback";
  NodeParam2[NodeParam2["DubDelayTone"] = 47] = "DubDelayTone";
  NodeParam2[NodeParam2["DubDelayMix"] = 48] = "DubDelayMix";
  NodeParam2[NodeParam2["EqBand1On"] = 49] = "EqBand1On";
  NodeParam2[NodeParam2["EqBand1Type"] = 50] = "EqBand1Type";
  NodeParam2[NodeParam2["EqBand1Freq"] = 51] = "EqBand1Freq";
  NodeParam2[NodeParam2["EqBand1Gain"] = 52] = "EqBand1Gain";
  NodeParam2[NodeParam2["EqBand1Q"] = 53] = "EqBand1Q";
  NodeParam2[NodeParam2["EqBand1ShelfSlope"] = 54] = "EqBand1ShelfSlope";
  NodeParam2[NodeParam2["EqBand1Slope"] = 55] = "EqBand1Slope";
  NodeParam2[NodeParam2["EqBand2On"] = 56] = "EqBand2On";
  NodeParam2[NodeParam2["EqBand2Type"] = 57] = "EqBand2Type";
  NodeParam2[NodeParam2["EqBand2Freq"] = 58] = "EqBand2Freq";
  NodeParam2[NodeParam2["EqBand2Gain"] = 59] = "EqBand2Gain";
  NodeParam2[NodeParam2["EqBand2Q"] = 60] = "EqBand2Q";
  NodeParam2[NodeParam2["EqBand2ShelfSlope"] = 61] = "EqBand2ShelfSlope";
  NodeParam2[NodeParam2["EqBand2Slope"] = 62] = "EqBand2Slope";
  NodeParam2[NodeParam2["EqBand3On"] = 63] = "EqBand3On";
  NodeParam2[NodeParam2["EqBand3Type"] = 64] = "EqBand3Type";
  NodeParam2[NodeParam2["EqBand3Freq"] = 65] = "EqBand3Freq";
  NodeParam2[NodeParam2["EqBand3Gain"] = 66] = "EqBand3Gain";
  NodeParam2[NodeParam2["EqBand3Q"] = 67] = "EqBand3Q";
  NodeParam2[NodeParam2["EqBand3ShelfSlope"] = 68] = "EqBand3ShelfSlope";
  NodeParam2[NodeParam2["EqBand3Slope"] = 69] = "EqBand3Slope";
  NodeParam2[NodeParam2["EqBand4On"] = 70] = "EqBand4On";
  NodeParam2[NodeParam2["EqBand4Type"] = 71] = "EqBand4Type";
  NodeParam2[NodeParam2["EqBand4Freq"] = 72] = "EqBand4Freq";
  NodeParam2[NodeParam2["EqBand4Gain"] = 73] = "EqBand4Gain";
  NodeParam2[NodeParam2["EqBand4Q"] = 74] = "EqBand4Q";
  NodeParam2[NodeParam2["EqBand4ShelfSlope"] = 75] = "EqBand4ShelfSlope";
  NodeParam2[NodeParam2["EqBand4Slope"] = 76] = "EqBand4Slope";
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
  EffectKind2[EffectKind2["Utility"] = 8] = "Utility";
  EffectKind2[EffectKind2["DubDelay"] = 9] = "DubDelay";
  EffectKind2[EffectKind2["Eq"] = 10] = "Eq";
  return EffectKind2;
})(EffectKind || {});
var EqBandType = /* @__PURE__ */ ((EqBandType2) => {
  EqBandType2[EqBandType2["Bell"] = 0] = "Bell";
  EqBandType2[EqBandType2["LowShelf"] = 1] = "LowShelf";
  EqBandType2[EqBandType2["HighShelf"] = 2] = "HighShelf";
  EqBandType2[EqBandType2["LowPass"] = 3] = "LowPass";
  EqBandType2[EqBandType2["HighPass"] = 4] = "HighPass";
  EqBandType2[EqBandType2["Notch"] = 5] = "Notch";
  return EqBandType2;
})(EqBandType || {});
var DubDelayDivision = /* @__PURE__ */ ((DubDelayDivision2) => {
  DubDelayDivision2[DubDelayDivision2["Sixteenth"] = 0] = "Sixteenth";
  DubDelayDivision2[DubDelayDivision2["Eighth"] = 1] = "Eighth";
  DubDelayDivision2[DubDelayDivision2["DottedEighth"] = 2] = "DottedEighth";
  DubDelayDivision2[DubDelayDivision2["Quarter"] = 3] = "Quarter";
  DubDelayDivision2[DubDelayDivision2["DottedQuarter"] = 4] = "DottedQuarter";
  DubDelayDivision2[DubDelayDivision2["Half"] = 5] = "Half";
  DubDelayDivision2[DubDelayDivision2["SixteenthTriplet"] = 6] = "SixteenthTriplet";
  DubDelayDivision2[DubDelayDivision2["EighthTriplet"] = 7] = "EighthTriplet";
  DubDelayDivision2[DubDelayDivision2["QuarterTriplet"] = 8] = "QuarterTriplet";
  DubDelayDivision2[DubDelayDivision2["HalfTriplet"] = 9] = "HalfTriplet";
  return DubDelayDivision2;
})(DubDelayDivision || {});
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
var InstrumentCommonParam = /* @__PURE__ */ ((InstrumentCommonParam2) => {
  InstrumentCommonParam2[InstrumentCommonParam2["Gain"] = 1e3] = "Gain";
  return InstrumentCommonParam2;
})(InstrumentCommonParam || {});
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
  InstrumentKind2[InstrumentKind2["PercSynth"] = 16] = "PercSynth";
  InstrumentKind2[InstrumentKind2["Edge"] = 17] = "Edge";
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
  BassParam2[BassParam2["EnvMod"] = 11] = "EnvMod";
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
  PulseParam2[PulseParam2["PumpRate"] = 9] = "PumpRate";
  PulseParam2[PulseParam2["PumpAttack"] = 10] = "PumpAttack";
  PulseParam2[PulseParam2["Pluck"] = 11] = "Pluck";
  PulseParam2[PulseParam2["Grit"] = 12] = "Grit";
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
var PercSynthParam = /* @__PURE__ */ ((PercSynthParam2) => {
  PercSynthParam2[PercSynthParam2["Tune"] = 0] = "Tune";
  PercSynthParam2[PercSynthParam2["Osc1Wave"] = 1] = "Osc1Wave";
  PercSynthParam2[PercSynthParam2["Osc1Level"] = 2] = "Osc1Level";
  PercSynthParam2[PercSynthParam2["Osc2Wave"] = 3] = "Osc2Wave";
  PercSynthParam2[PercSynthParam2["Osc2Pitch"] = 4] = "Osc2Pitch";
  PercSynthParam2[PercSynthParam2["Osc2Level"] = 5] = "Osc2Level";
  PercSynthParam2[PercSynthParam2["Osc2Fm"] = 6] = "Osc2Fm";
  PercSynthParam2[PercSynthParam2["PitchEnv"] = 7] = "PitchEnv";
  PercSynthParam2[PercSynthParam2["PitchDecay"] = 8] = "PitchDecay";
  PercSynthParam2[PercSynthParam2["Osc2Env"] = 9] = "Osc2Env";
  PercSynthParam2[PercSynthParam2["Noise"] = 10] = "Noise";
  PercSynthParam2[PercSynthParam2["NoiseTone"] = 11] = "NoiseTone";
  PercSynthParam2[PercSynthParam2["NoiseDecay"] = 12] = "NoiseDecay";
  PercSynthParam2[PercSynthParam2["FilterType"] = 13] = "FilterType";
  PercSynthParam2[PercSynthParam2["Cutoff"] = 14] = "Cutoff";
  PercSynthParam2[PercSynthParam2["Resonance"] = 15] = "Resonance";
  PercSynthParam2[PercSynthParam2["FilterEnv"] = 16] = "FilterEnv";
  PercSynthParam2[PercSynthParam2["FilterDecay"] = 17] = "FilterDecay";
  PercSynthParam2[PercSynthParam2["Attack"] = 18] = "Attack";
  PercSynthParam2[PercSynthParam2["Decay"] = 19] = "Decay";
  PercSynthParam2[PercSynthParam2["Sustain"] = 20] = "Sustain";
  PercSynthParam2[PercSynthParam2["Release"] = 21] = "Release";
  PercSynthParam2[PercSynthParam2["Drive"] = 22] = "Drive";
  return PercSynthParam2;
})(PercSynthParam || {});
var EdgeParam = /* @__PURE__ */ ((EdgeParam2) => {
  EdgeParam2[EdgeParam2["Tune"] = 0] = "Tune";
  EdgeParam2[EdgeParam2["Shape"] = 1] = "Shape";
  EdgeParam2[EdgeParam2["Bright"] = 2] = "Bright";
  EdgeParam2[EdgeParam2["BrightEnv"] = 3] = "BrightEnv";
  EdgeParam2[EdgeParam2["Detune"] = 4] = "Detune";
  EdgeParam2[EdgeParam2["PitchEnv"] = 5] = "PitchEnv";
  EdgeParam2[EdgeParam2["PitchDecay"] = 6] = "PitchDecay";
  EdgeParam2[EdgeParam2["Routing"] = 7] = "Routing";
  EdgeParam2[EdgeParam2["FilterA"] = 8] = "FilterA";
  EdgeParam2[EdgeParam2["FilterB"] = 9] = "FilterB";
  EdgeParam2[EdgeParam2["Cutoff"] = 10] = "Cutoff";
  EdgeParam2[EdgeParam2["Spread"] = 11] = "Spread";
  EdgeParam2[EdgeParam2["Resonance"] = 12] = "Resonance";
  EdgeParam2[EdgeParam2["FilterEnv"] = 13] = "FilterEnv";
  EdgeParam2[EdgeParam2["ToneDecay"] = 14] = "ToneDecay";
  EdgeParam2[EdgeParam2["LfoRate"] = 15] = "LfoRate";
  EdgeParam2[EdgeParam2["LfoPitch"] = 16] = "LfoPitch";
  EdgeParam2[EdgeParam2["LfoFilter"] = 17] = "LfoFilter";
  EdgeParam2[EdgeParam2["Attack"] = 18] = "Attack";
  EdgeParam2[EdgeParam2["Decay"] = 19] = "Decay";
  EdgeParam2[EdgeParam2["Sustain"] = 20] = "Sustain";
  EdgeParam2[EdgeParam2["Release"] = 21] = "Release";
  return EdgeParam2;
})(EdgeParam || {});
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

// src/runtime/midiRoute.ts
var finite = (n) => typeof n === "number" && Number.isFinite(n);
function normalizeMidiRoute(route, known) {
  if (!route) return null;
  if (route.kind !== "sample" && route.kind !== "instrument") return null;
  if (!finite(route.id) || !finite(route.unityNote)) return null;
  if (!(route.kind === "sample" ? known.sample(route.id) : known.instrument(route.id))) return null;
  const out = { kind: route.kind, id: route.id, unityNote: Math.min(127, Math.max(0, Math.round(route.unityNote))) };
  if (route.holdUntilKeyUp) out.holdUntilKeyUp = true;
  if (finite(route.gain)) out.gain = Math.min(1, Math.max(0, route.gain));
  if (finite(route.chokeGroup) && route.chokeGroup >= 1) out.chokeGroup = Math.floor(route.chokeGroup);
  if (finite(route.channel) && route.channel >= 0 && route.channel <= 15) out.channel = Math.floor(route.channel);
  if (route.kind === "instrument" && route.fixedPitch) out.fixedPitch = true;
  if (route.kind === "sample") {
    if (finite(route.bus) && route.bus >= 0) out.bus = Math.floor(route.bus);
    if (route.chop) out.chop = true;
    if (route.loop) out.loop = true;
    if (route.mono) out.mono = true;
  }
  return out;
}
function sameMidiRoute(a, b) {
  return JSON.stringify(a) === JSON.stringify(b);
}

// src/runtime/sequence.ts
var MAX_SEQUENCE_EVENTS = 2e5;
var finite2 = (n) => typeof n === "number" && Number.isFinite(n);
function normalizeSequence(seq, known) {
  if (!seq || !finite2(seq.lengthBeats) || seq.lengthBeats <= 0 || !Array.isArray(seq.events)) return null;
  let budget = MAX_SEQUENCE_EVENTS;
  const events = cleanEvents(seq.events, seq.lengthBeats, known, budget);
  budget -= events.length;
  const loops = [];
  if (Array.isArray(seq.loops)) {
    for (const l of seq.loops) {
      if (!l || !finite2(l.lengthBeats) || l.lengthBeats <= 0 || !Array.isArray(l.events)) continue;
      const cleaned = cleanEvents(l.events, l.lengthBeats, known, budget);
      budget -= cleaned.length;
      if (cleaned.length) loops.push({ lengthBeats: l.lengthBeats, events: cleaned });
    }
  }
  if (events.length === 0 && loops.length === 0) return null;
  return { lengthBeats: seq.lengthBeats, events, ...loops.length ? { loops } : {} };
}
function cleanEvents(list, length, known, limit) {
  const events = [];
  for (const e of list) {
    if (events.length >= limit) break;
    if (!e || !finite2(e.beat) || e.beat < 0 || e.beat >= length) continue;
    const dur = (v) => finite2(v) && v > 0 ? { durationBeats: v } : {};
    const choke = (v) => finite2(v) && v >= 1 ? { chokeGroup: Math.floor(v) } : {};
    const num = (key, v) => finite2(v) ? { [key]: v } : {};
    if (e.kind === "sample") {
      if (!finite2(e.sample) || !known.sample(e.sample)) continue;
      events.push({
        beat: e.beat,
        kind: "sample",
        sample: e.sample,
        ...finite2(e.bus) && e.bus >= 0 ? { bus: Math.floor(e.bus) } : {},
        ...num("gain", e.gain),
        ...finite2(e.pan) ? { pan: Math.min(1, Math.max(-1, e.pan)) } : {},
        ...num("pitch", e.pitch),
        ...finite2(e.region) && e.region >= 0 ? { region: Math.floor(e.region) } : {},
        ...e.loop ? { loop: true } : {},
        ...dur(e.durationBeats),
        ...choke(e.chokeGroup),
        ...finite2(e.track) && e.track >= 0 ? { track: Math.floor(e.track) } : {},
        ...e.mono ? { mono: true } : {}
      });
    } else if (e.kind === "instrument") {
      if (!finite2(e.instrument) || !known.instrument(e.instrument)) continue;
      events.push({
        beat: e.beat,
        kind: "instrument",
        instrument: e.instrument,
        ...finite2(e.velocity) ? { velocity: Math.min(1, Math.max(0, e.velocity)) } : {},
        ...finite2(e.pan) ? { pan: Math.min(1, Math.max(-1, e.pan)) } : {},
        ...num("pitch", e.pitch),
        ...dur(e.durationBeats),
        ...choke(e.chokeGroup),
        ...e.tie ? { tie: true } : {}
      });
    } else if (e.kind === "param") {
      const t = e.target;
      if (!finite2(e.param) || !finite2(e.value) || !t) continue;
      if ("effect" in t) {
        if (!known.effect(t.effect.bus, t.effect.effect)) continue;
      } else if ("instrument" in t) {
        if (!known.instrument(t.instrument)) continue;
      } else if ("sample" in t) {
        if (!known.sample(t.sample)) continue;
      } else if ("bus" in t) {
        if (!known.bus?.(t.bus)) continue;
      } else continue;
      events.push({
        beat: e.beat,
        kind: "param",
        target: t,
        param: e.param,
        value: e.value,
        ...num("from", e.from),
        ...finite2(e.rampBeats) && e.rampBeats > 0 ? { rampBeats: e.rampBeats } : {}
      });
    }
  }
  events.sort((a, b) => a.beat - b.beat);
  return events;
}

// src/runtime/scale.ts
function logFreqToUnit(hz, minHz, maxHz) {
  return Math.log(hz / minHz) / Math.log(maxHz / minHz);
}
function dbToUnit(db, minDb, maxDb) {
  return (db - minDb) / (maxDb - minDb);
}

// src/runtime/meters.ts
var CLIP_THRESHOLD = 1;
function decodeMeters(raw) {
  return {
    masterIn: raw[0],
    masterOut: raw[1],
    gainReductionDb: raw[2],
    buses: raw.subarray(3)
  };
}
function emptyMeters(maxBuses) {
  return { masterIn: 0, masterOut: 0, gainReductionDb: 0, buses: new Float32Array(maxBuses) };
}
function busPeak(snapshot, bus) {
  return bus >= 1 && bus <= snapshot.buses.length ? snapshot.buses[bus - 1] : 0;
}
var METER_LED_DB = [-48, -36, -24, -18, -12, -8, -4, -2, -0.5, 0];
var METER_RED_LEDS = 3;
var METER_MIN_DB = METER_LED_DB[0];
var METER_MAX_DB = 0;
var METER_RED_DB = METER_LED_DB[METER_LED_DB.length - METER_RED_LEDS];
function peakToUnit(peak) {
  return Math.min(1, Math.max(0, dbToUnit(peakToDb(peak), METER_MIN_DB, METER_MAX_DB)));
}
function peakToDb(peak) {
  return peak > 1e-6 ? 20 * Math.log10(peak) : -120;
}
function metersLitLeds(peak) {
  const db = peakToDb(peak) + 1e-4;
  let n = 0;
  while (n < METER_LED_DB.length && db >= METER_LED_DB[n]) n++;
  return n;
}
var MeterTracker = class {
  constructor(maxBuses, options = {}) {
    __publicField(this, "clipHoldMs");
    __publicField(this, "fallDbPerSec");
    __publicField(this, "level", 0);
    __publicField(this, "masterClipAt", -Infinity);
    __publicField(this, "gainReductionDb", 0);
    __publicField(this, "lastMs", null);
    __publicField(this, "busClipAt");
    this.clipHoldMs = options.clipHoldMs ?? 1500;
    this.fallDbPerSec = options.fallDbPerSec ?? 24;
    this.busClipAt = new Float64Array(maxBuses).fill(-Infinity);
  }
  update(snapshot, nowMs) {
    const dt = this.lastMs === null ? 0 : Math.max(0, nowMs - this.lastMs) / 1e3;
    this.lastMs = nowMs;
    const decayed = this.level * Math.pow(10, -this.fallDbPerSec * dt / 20);
    this.level = Math.max(snapshot.masterIn, decayed < 1e-4 ? 0 : decayed);
    this.gainReductionDb = snapshot.gainReductionDb;
    if (snapshot.masterIn >= CLIP_THRESHOLD) this.masterClipAt = nowMs;
    const n = Math.min(this.busClipAt.length, snapshot.buses.length);
    for (let i = 0; i < n; i++) {
      if (snapshot.buses[i] >= CLIP_THRESHOLD) this.busClipAt[i] = nowMs;
    }
  }
  /** Master level (linear peak, pre-limiter) with fall-off applied. */
  masterLevel() {
    return this.level;
  }
  masterLitLeds() {
    return metersLitLeds(this.level);
  }
  /** Limiter gain reduction in the latest interval, dB (<= 0). */
  masterGainReductionDb() {
    return this.gainReductionDb;
  }
  masterClipping(nowMs) {
    return nowMs - this.masterClipAt < this.clipHoldMs;
  }
  busClipping(bus, nowMs) {
    return bus >= 1 && bus <= this.busClipAt.length && nowMs - this.busClipAt[bus - 1] < this.clipHoldMs;
  }
  reset() {
    this.level = 0;
    this.masterClipAt = -Infinity;
    this.gainReductionDb = 0;
    this.lastMs = null;
    this.busClipAt.fill(-Infinity);
  }
};

// src/runtime/spectrum.ts
function spectrumPoints(a, width, height, minDb, maxDb, out) {
  for (let c = 0; c < a.columns; c++) {
    out[2 * c] = logFreqToUnit(a.frequencies[c], a.minHz, a.maxHz) * width;
    out[2 * c + 1] = height * (1 - Math.min(1, Math.max(0, dbToUnit(a.db[c], minDb, maxDb))));
  }
}
var SpectrumAnalyzer = class {
  constructor(options = {}) {
    __publicField(this, "columns");
    __publicField(this, "minHz");
    __publicField(this, "maxHz");
    __publicField(this, "floorDb");
    /** Centre frequency of each column, Hz (log-spaced). */
    __publicField(this, "frequencies");
    /** Smoothed level per column in dBFS (a full-scale sine reads about 0). Updated in place. */
    __publicField(this, "db");
    __publicField(this, "n");
    __publicField(this, "fallDbPerSec");
    __publicField(this, "window");
    __publicField(this, "re");
    __publicField(this, "im");
    __publicField(this, "cos");
    __publicField(this, "sin");
    __publicField(this, "rev");
    __publicField(this, "scale");
    __publicField(this, "binHz", 0);
    // Per column: the range of FFT bins it covers (lo..hi inclusive) -- or, when narrower than one
    // bin, a fractional position to interpolate at (hi < lo).
    __publicField(this, "lo");
    __publicField(this, "hi");
    __publicField(this, "frac");
    __publicField(this, "mapRate", 0);
    const n = this.n = options.fftSize ?? 2048;
    if (n < 16 || (n & n - 1) !== 0) throw new Error("fftSize must be a power of two >= 16");
    this.columns = options.columns ?? 160;
    this.minHz = options.minHz ?? 20;
    this.maxHz = options.maxHz ?? 2e4;
    this.floorDb = options.floorDb ?? -100;
    this.fallDbPerSec = options.fallDbPerSec ?? 40;
    this.window = new Float64Array(n);
    let sum = 0;
    for (let i = 0; i < n; i++) sum += this.window[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / n);
    this.scale = 2 / sum;
    this.re = new Float64Array(n);
    this.im = new Float64Array(n);
    this.cos = new Float64Array(n / 2);
    this.sin = new Float64Array(n / 2);
    for (let i = 0; i < n / 2; i++) {
      this.cos[i] = Math.cos(2 * Math.PI * i / n);
      this.sin[i] = -Math.sin(2 * Math.PI * i / n);
    }
    this.rev = new Uint32Array(n);
    const bits = Math.log2(n);
    for (let i = 0; i < n; i++) {
      let r = 0;
      for (let b = 0; b < bits; b++) if (i & 1 << b) r |= 1 << bits - 1 - b;
      this.rev[i] = r;
    }
    this.frequencies = new Float32Array(this.columns);
    this.db = new Float32Array(this.columns).fill(this.floorDb);
    this.lo = new Int32Array(this.columns);
    this.hi = new Int32Array(this.columns);
    this.frac = new Float32Array(this.columns);
    const ratio = Math.log(this.maxHz / this.minHz);
    for (let c = 0; c < this.columns; c++) {
      this.frequencies[c] = this.minHz * Math.exp(ratio * (c + 0.5) / this.columns);
    }
  }
  mapColumns(sampleRate) {
    this.mapRate = sampleRate;
    this.binHz = sampleRate / this.n;
    const half = this.n / 2 - 1;
    const ratio = Math.log(this.maxHz / this.minHz);
    for (let c = 0; c < this.columns; c++) {
      const f0 = this.minHz * Math.exp(ratio * c / this.columns);
      const f1 = this.minHz * Math.exp(ratio * (c + 1) / this.columns);
      const lo = Math.min(half, Math.ceil(f0 / this.binHz));
      const hi = Math.min(half, Math.floor(f1 / this.binHz));
      this.lo[c] = lo;
      this.hi[c] = hi;
      this.frac[c] = this.frequencies[c] / this.binHz;
    }
  }
  /** Analyses the last `fftSize` samples of `samples` and folds them into `db` (rises instantly,
   * falls at `fallDbPerSec` given `dtSec` since the previous update). */
  push(samples, sampleRate, dtSec) {
    if (samples.length < this.n) return;
    if (sampleRate !== this.mapRate) this.mapColumns(sampleRate);
    const n = this.n, off = samples.length - n;
    for (let i = 0; i < n; i++) {
      const r = this.rev[i];
      this.re[r] = samples[off + i] * this.window[i];
      this.im[r] = 0;
    }
    for (let size = 2; size <= n; size <<= 1) {
      const halfSize = size >> 1, step = n / size;
      for (let start = 0; start < n; start += size) {
        for (let j = 0, k = 0; j < halfSize; j++, k += step) {
          const a = start + j, b = a + halfSize;
          const tr = this.re[b] * this.cos[k] - this.im[b] * this.sin[k];
          const ti = this.re[b] * this.sin[k] + this.im[b] * this.cos[k];
          this.re[b] = this.re[a] - tr;
          this.im[b] = this.im[a] - ti;
          this.re[a] += tr;
          this.im[a] += ti;
        }
      }
    }
    const fall = this.fallDbPerSec * Math.max(0, dtSec);
    for (let c = 0; c < this.columns; c++) {
      let m2;
      if (this.hi[c] >= this.lo[c]) {
        m2 = 0;
        for (let b = this.lo[c]; b <= this.hi[c]; b++) m2 = Math.max(m2, this.re[b] * this.re[b] + this.im[b] * this.im[b]);
      } else {
        const p = this.frac[c], b0 = Math.floor(p), t = p - b0;
        const a = this.re[b0] * this.re[b0] + this.im[b0] * this.im[b0];
        const b = this.re[b0 + 1] * this.re[b0 + 1] + this.im[b0 + 1] * this.im[b0 + 1];
        m2 = a + (b - a) * t;
      }
      const level = m2 > 0 ? 10 * Math.log10(m2) + 20 * Math.log10(this.scale) : this.floorDb;
      const clamped = Math.max(this.floorDb, level);
      this.db[c] = Math.max(clamped, this.db[c] - fall);
    }
  }
  /** Lets the display fall away when no new audio arrives (silence, stopped transport). */
  decay(dtSec) {
    const fall = this.fallDbPerSec * Math.max(0, dtSec);
    for (let c = 0; c < this.columns; c++) this.db[c] = Math.max(this.floorDb, this.db[c] - fall);
  }
  reset() {
    this.db.fill(this.floorDb);
  }
};
var ANALYSIS_TAP_SLOTS = 4;
var SpectrumFeed = class {
  constructor(source, bus, effect, post, analyzer) {
    __publicField(this, "source", source);
    __publicField(this, "analyzer", analyzer);
    __publicField(this, "pending", null);
    __publicField(this, "pendingRate", 0);
    __publicField(this, "lastMs", null);
    __publicField(this, "settled", false);
    __publicField(this, "unsubscribe");
    /** The tap slot this feed owns (null if the engine had none free: the display just stays empty). */
    __publicField(this, "slot");
    this.unsubscribe = source.onAnalysis((f) => {
      if (f.slot !== this.slot) return;
      this.pending = f.samples;
      this.pendingRate = f.sampleRate;
    });
    this.slot = source.openAnalysisTap(bus, effect, post);
  }
  frame(nowMs) {
    const dt = this.lastMs === null ? 0 : Math.min(0.25, Math.max(0, (nowMs - this.lastMs) / 1e3));
    this.lastMs = nowMs;
    if (this.pending) {
      this.analyzer.push(this.pending, this.pendingRate, dt);
      this.pending = null;
      this.settled = false;
      return true;
    }
    if (this.settled) return false;
    this.analyzer.decay(dt);
    let floor = true;
    for (let i = 0; i < this.analyzer.columns; i++) if (this.analyzer.db[i] > this.analyzer.floorDb) {
      floor = false;
      break;
    }
    this.settled = floor;
    return true;
  }
  dispose() {
    this.unsubscribe();
    if (this.slot !== null) this.source.closeAnalysisTap(this.slot);
    this.pending = null;
  }
};

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

// src/runtime/eq.ts
var EQ_BAND_COUNT = 4;
var EQ_MIN_HZ = 20;
var EQ_MAX_HZ = 2e4;
var EQ_MAX_GAIN_DB = 18;
var EQ_MIN_Q = 0.1;
var EQ_MAX_Q = 18;
var EQ_MIN_SHELF_SLOPE = 0.1;
var EQ_MAX_SHELF_SLOPE = 1;
function defaultEqBands() {
  const mk = (type, frequency) => ({
    enabled: false,
    type,
    frequency,
    gain: 0,
    q: 0.707,
    shelfSlope: 1,
    slope: 12
  });
  return [
    mk(1 /* LowShelf */, 100),
    mk(0 /* Bell */, 500),
    mk(0 /* Bell */, 2e3),
    mk(2 /* HighShelf */, 8e3)
  ];
}
var FIELD_OFFSET = {
  enabled: 0,
  type: 1,
  frequency: 2,
  gain: 3,
  q: 4,
  shelfSlope: 5,
  slope: 6
};
function eqBandParam(band, field) {
  return 49 /* EqBand1On */ + band * 7 + FIELD_OFFSET[field];
}
function eqBandUses(type, field) {
  switch (field) {
    case "gain":
      return type === 0 /* Bell */ || type === 1 /* LowShelf */ || type === 2 /* HighShelf */;
    case "q":
      return type === 0 /* Bell */ || type === 5 /* Notch */;
    case "shelfSlope":
      return type === 1 /* LowShelf */ || type === 2 /* HighShelf */;
    case "slope":
      return type === 3 /* LowPass */ || type === 4 /* HighPass */;
    default:
      return true;
  }
}
function eqBandToParams(band, b) {
  return [
    [eqBandParam(band, "enabled"), b.enabled ? 1 : 0],
    [eqBandParam(band, "type"), b.type],
    [eqBandParam(band, "frequency"), b.frequency],
    [eqBandParam(band, "gain"), b.gain],
    [eqBandParam(band, "q"), b.q],
    [eqBandParam(band, "shelfSlope"), b.shelfSlope],
    [eqBandParam(band, "slope"), b.slope]
  ];
}
var clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
function rbj(type, omega, q, gainDb, s) {
  const sinO = Math.sin(omega), cosO = Math.cos(omega);
  const A = Math.pow(10, gainDb / 40);
  let b0, b1, b2, a0, a1, a2;
  switch (type) {
    case 0 /* Bell */: {
      const alpha = sinO / (2 * q);
      b0 = 1 + alpha * A;
      b1 = -2 * cosO;
      b2 = 1 - alpha * A;
      a0 = 1 + alpha / A;
      a1 = -2 * cosO;
      a2 = 1 - alpha / A;
      break;
    }
    case 1 /* LowShelf */:
    case 2 /* HighShelf */: {
      const t = 2 * Math.sqrt(A) * (sinO / 2) * Math.sqrt((A + 1 / A) * (1 / s - 1) + 2);
      if (type === 1 /* LowShelf */) {
        b0 = A * (A + 1 - (A - 1) * cosO + t);
        b1 = 2 * A * (A - 1 - (A + 1) * cosO);
        b2 = A * (A + 1 - (A - 1) * cosO - t);
        a0 = A + 1 + (A - 1) * cosO + t;
        a1 = -2 * (A - 1 + (A + 1) * cosO);
        a2 = A + 1 + (A - 1) * cosO - t;
      } else {
        b0 = A * (A + 1 + (A - 1) * cosO + t);
        b1 = -2 * A * (A - 1 + (A + 1) * cosO);
        b2 = A * (A + 1 + (A - 1) * cosO - t);
        a0 = A + 1 - (A - 1) * cosO + t;
        a1 = 2 * (A - 1 - (A + 1) * cosO);
        a2 = A + 1 - (A - 1) * cosO - t;
      }
      break;
    }
    case 3 /* LowPass */: {
      const alpha = sinO / (2 * q);
      b0 = (1 - cosO) / 2;
      b1 = 1 - cosO;
      b2 = b0;
      a0 = 1 + alpha;
      a1 = -2 * cosO;
      a2 = 1 - alpha;
      break;
    }
    case 4 /* HighPass */: {
      const alpha = sinO / (2 * q);
      b0 = (1 + cosO) / 2;
      b1 = -(1 + cosO);
      b2 = b0;
      a0 = 1 + alpha;
      a1 = -2 * cosO;
      a2 = 1 - alpha;
      break;
    }
    default: {
      const alpha = sinO / (2 * q);
      b0 = 1;
      b1 = -2 * cosO;
      b2 = 1;
      a0 = 1 + alpha;
      a1 = -2 * cosO;
      a2 = 1 - alpha;
    }
  }
  return { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 };
}
function eqBandSections(b, sampleRate) {
  const freq = Math.min(clamp(b.frequency, EQ_MIN_HZ, EQ_MAX_HZ), sampleRate * 0.49);
  const omega = 2 * Math.PI * freq / sampleRate;
  const pass = b.type === 3 /* LowPass */ || b.type === 4 /* HighPass */;
  if (pass && b.slope >= 18) {
    return [rbj(b.type, omega, 0.5411961, 0, 1), rbj(b.type, omega, 1.30656296, 0, 1)];
  }
  return [rbj(
    b.type,
    omega,
    pass ? 0.70710678 : clamp(b.q, EQ_MIN_Q, EQ_MAX_Q),
    clamp(b.gain, -EQ_MAX_GAIN_DB, EQ_MAX_GAIN_DB),
    clamp(b.shelfSlope, EQ_MIN_SHELF_SLOPE, EQ_MAX_SHELF_SLOPE)
  )];
}
function magnitudeDb(k, omega) {
  const c1 = Math.cos(omega), c2 = Math.cos(2 * omega), s1 = Math.sin(omega), s2 = Math.sin(2 * omega);
  const nr = k.b0 + k.b1 * c1 + k.b2 * c2, ni = -(k.b1 * s1 + k.b2 * s2);
  const dr = 1 + k.a1 * c1 + k.a2 * c2, di = -(k.a1 * s1 + k.a2 * s2);
  return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
}
function eqBandResponseDb(b, frequency, sampleRate) {
  if (!b.enabled) return 0;
  const omega = 2 * Math.PI * frequency / sampleRate;
  let db = 0;
  for (const k of eqBandSections(b, sampleRate)) db += magnitudeDb(k, omega);
  return db;
}
function eqResponseDb(bands, frequency, sampleRate) {
  let db = 0;
  for (const b of bands) db += eqBandResponseDb(b, frequency, sampleRate);
  return db;
}
function eqCurve(bands, sampleRate, points = 256, minHz = EQ_MIN_HZ, maxHz = EQ_MAX_HZ) {
  const frequencies = new Float64Array(points);
  const combinedDb = new Float64Array(points);
  const bandDb = bands.map(() => new Float64Array(points));
  const ratio = Math.log(maxHz / minHz);
  for (let i = 0; i < points; i++) {
    const f = minHz * Math.exp(ratio * i / Math.max(1, points - 1));
    frequencies[i] = f;
    bands.forEach((b, j) => {
      const db = eqBandResponseDb(b, f, sampleRate);
      bandDb[j][i] = db;
      combinedDb[i] += db;
    });
  }
  return { frequencies, combinedDb, bandDb };
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

// src/runtime/hostTransport.ts
function hostBeatAt(transport, time) {
  return transport.beat + (time - transport.time) * (transport.bpm / 60);
}
function hostTimeAt(transport, beat) {
  return transport.time + (beat - transport.beat) * (60 / transport.bpm);
}
function isHostDiscontinuity(prev, next) {
  return !prev || prev.epoch !== next.epoch || prev.playing !== next.playing;
}
function hasEmbeddedHost() {
  return Boolean(globalThis.__webdspHost);
}

// src/runtime/hostState.ts
function getHostState() {
  const host = globalThis.__webdspHost;
  return host?.state ?? null;
}

// src/runtime/hostFiles.ts
function getHostFiles() {
  const host = globalThis.__webdspHost;
  return host?.files ?? null;
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
    __publicField(this, "meters", emptyMeters(MAX_TRACK_BUSES));
    __publicField(this, "meterListeners", /* @__PURE__ */ new Set());
    __publicField(this, "analysisListeners", /* @__PURE__ */ new Set());
    __publicField(this, "analysisSlotsInUse", Array(ANALYSIS_TAP_SLOTS).fill(false));
    __publicField(this, "diagnosticsListeners", /* @__PURE__ */ new Set());
    __publicField(this, "voiceEndedListeners", /* @__PURE__ */ new Set());
    __publicField(this, "hostTransport", null);
    __publicField(this, "hostTransportListeners", /* @__PURE__ */ new Set());
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
    const warning = hostProtocolWarning(ready.protocolVersion);
    if (warning) console.warn(warning);
    const capabilities = {
      ...ready.protocolVersion !== void 0 ? { hostProtocolVersion: ready.protocolVersion } : {},
      ...ready.midiRouting ? { midiRouting: true } : {},
      ...ready.nativeSequence ? { nativeSequence: true } : {},
      ...ready.sequenceLoops ? { sequenceLoops: true } : {},
      sampleRate: ready.sampleRate,
      outputChannels: ready.outputChannels,
      maxVoices: ready.maxVoices,
      renderQuantumFrames: ready.renderQuantumFrames,
      maxCaptureSeconds: ready.maxCaptureSeconds,
      instruments: ready.instruments,
      effects: ready.effects,
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
  /** Latest peak snapshot (see MeterSnapshot); all zero until the first "meters" event. */
  getMeters() {
    return this.meters;
  }
  /** Subscribes to peak snapshots, posted ~47 Hz while there is signal and silent otherwise.
   * Each snapshot covers the interval since the previous one, so no peak is missed. Pair with
   * `MeterTracker` for hold/decay display state. */
  onMeters(fn) {
    this.meterListeners.add(fn);
    return () => this.meterListeners.delete(fn);
  }
  /** Display-only spectrum feed. Opens a tap slot on one effect instance: its INPUT (`post` false)
   * or its OUTPUT (`post` true), so a display can show what goes in and what comes out. Works
   * while the effect is bypassed (output = input). Returns the slot, or null when all
   * ANALYSIS_TAP_SLOTS are in use. While open, `onAnalysis` listeners get that slot's latest mono
   * window ~23 times a second (`frame.slot` says which); hand it to a `SpectrumFeed`
   * (spectrum.ts), which does all of this. Costs the audio thread one copy per block per open
   * slot, nothing when none are open. */
  openAnalysisTap(bus, effect, post = false) {
    const slot = this.analysisSlotsInUse.indexOf(false);
    if (slot < 0) return null;
    this.analysisSlotsInUse[slot] = true;
    this.bridge.send({ type: "set-analysis-tap", slot, bus, effect, post });
    return slot;
  }
  closeAnalysisTap(slot) {
    if (!this.analysisSlotsInUse[slot]) return;
    this.analysisSlotsInUse[slot] = false;
    this.bridge.send({ type: "set-analysis-tap", slot, bus: 0, effect: 0, post: false });
  }
  onAnalysis(fn) {
    this.analysisListeners.add(fn);
    return () => this.analysisListeners.delete(fn);
  }
  /** Configures the master limiter (a thin wrapper over setNodeParameter on MASTER_BUS). The
   * limiter is on by default with a -0.3 dBFS ceiling and 100 ms release. */
  setMasterLimiter(settings) {
    if (settings.enabled !== void 0) this.setNodeParameter(MASTER_BUS, 38 /* LimiterEnable */, settings.enabled ? 1 : 0);
    if (settings.ceilingDb !== void 0) this.setNodeParameter(MASTER_BUS, 39 /* LimiterCeiling */, settings.ceilingDb);
    if (settings.releaseMs !== void 0) this.setNodeParameter(MASTER_BUS, 40 /* LimiterRelease */, settings.releaseMs);
  }
  /** The host's own transport, or null when the runtime has none (a plain browser). See hostTransport.ts. */
  getHostTransport() {
    return this.hostTransport;
  }
  /** Subscribes to host transport updates: every start, stop, locate, loop change and tempo change, plus a
   * periodic refresh while playing. Never fires in a browser. Returns an unsubscribe function. */
  onHostTransport(fn) {
    this.hostTransportListeners.add(fn);
    return () => this.hostTransportListeners.delete(fn);
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
  // --- effect parameters and modulation ---
  // Effects use the same LFO bank as synths (docs/modulation.md): the app owns the BASE value
  // (setEffectParameter, e.g. driven by automation) and the engine adds the LFO stack on top.
  /** The parameters of an effect kind, straight from the engine (ranges, defaults, units and
   * whether an LFO can target them). */
  getEffectParams(kind) {
    return this.capabilities.effects.find((k) => k.kind === kind)?.params ?? [];
  }
  /** The params of `kind` an LFO can target. Build a modulation target picker from this. */
  getEffectModulationTargets(kind) {
    return this.getEffectParams(kind).filter((p) => p.modulation !== "none");
  }
  /** Enables or edits LFO `slot` (0..maxLfos-1) on an effect instance. Same semantics as
   * setInstrumentLfo(): editing keeps the phase running, an invalid target leaves the slot off,
   * several LFOs may share a target (they add). */
  setEffectLfo(bus, effect, slot, lfo) {
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
      waveform: lfo.waveform ?? 0 /* Sine */,
      sync: lfo.sync ?? false,
      rateHz: lfo.rateHz ?? 1,
      division: lfo.division ?? (quarter >= 0 ? quarter : 0),
      phase: lfo.phase ?? 0,
      amount: lfo.amount,
      target: lfo.target
    });
  }
  /** Removes LFO `slot` from an effect instance; its target goes back to the plain base value. */
  clearEffectLfo(bus, effect, slot) {
    if (!this.chainFor(bus).some((e) => e.id === effect)) return;
    if (!Number.isInteger(slot) || slot < 0 || slot >= this.capabilities.modulation.maxLfos) return;
    this.bridge.send({
      type: "set-effect-lfo",
      bus,
      effect,
      slot,
      enabled: false,
      waveform: 0,
      sync: false,
      rateHz: 1,
      division: 0,
      phase: 0,
      amount: 0,
      target: 0
    });
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
  /** Declares what incoming MIDI notes play, for a host that receives MIDI itself and can play it without the page (see midiRoute.ts and
   * `capabilities.midiRouting`); null clears it. A no-op where the host cannot route (a browser): the application then plays notes
   * itself. Unusable routes (an unknown sample or instrument, non-finite numbers) are treated as null. Re-declare when what a key
   * should play changes (a different track selected, a sample reassigned); the route holds until replaced or the engine is reset. */
  setMidiRoute(route) {
    if (!this.capabilities.midiRouting) return;
    const clean = normalizeMidiRoute(route, { sample: (id) => this.samples.has(id), instrument: (id) => this.instruments.has(id) });
    this.bridge.send({ type: "set-midi-route", route: clean });
  }
  /** Hands a host that can play it (`capabilities.nativeSequence`) the song as a Sequence: one cycle of timed events in quarter notes, which the
   * host plays against ITS OWN transport on its audio thread (locked to the DAW's position and tempo, loops, locates and offline bounce), with the
   * page's window open or closed (see sequence.ts, docs/native-sequence.md). Replaces any earlier sequence at the next block; null clears it. A
   * no-op where the host cannot (a browser): the application then sequences from the page as it always did. Events for samples, instruments or
   * effects the engine does not know are dropped, so declare the sequence after creating what it uses and again when those change. */
  setSequence(sequence) {
    if (!this.capabilities.nativeSequence) return;
    const clean = normalizeSequence(sequence, {
      sample: (id) => this.samples.has(id),
      instrument: (id) => this.instruments.has(id),
      effect: (bus, effect) => this.chainFor(bus).some((e) => e.id === effect),
      bus: (bus) => this.knowsBusTarget(bus)
    });
    this.bridge.send({ type: "set-sequence", sequence: clean });
  }
  /** Whether a scheduled parameter event may address this bus's mixer parameters: the host implements them (protocol 4; the
   * browser worklet does not) and the bus exists (MASTER_BUS, or one createBus() handed out). */
  knowsBusTarget(bus) {
    if ((this.capabilities.hostProtocolVersion ?? 0) < 4) return false;
    return Number.isInteger(bus) && bus >= 0 && bus < this.nextTrackBus;
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
  /** Schedules parameter writes on the audio clock: a step at `time`, or a linear ramp reaching
   * `value` at `time + ramp`. They write the BASE value (engine LFOs ride on top), apply at the
   * start of the 128-frame block containing `time` (about 2.7 ms), before that block's notes, and
   * are dropped by cancelScheduled() like notes (a ramp already running keeps going until a direct
   * setter, or its end). A direct setEffectParameter / setInstrumentParameter / setSampleParameter
   * on the same parameter cancels its ramp. Events for unknown targets or with non-finite numbers
   * are ignored. A `{ bus }` target writes a bus's mixer parameters (BusGain, BusPan, BusSendAmount, BusMainGain) and needs a
   * native host with protocol 4; elsewhere it is dropped. See docs/scheduled-params.md. */
  scheduleParams(events) {
    const wire = [];
    for (const e of events) {
      const ramp = e.ramp ?? 0;
      if (!Number.isFinite(e.time) || !Number.isFinite(e.value) || !Number.isFinite(ramp) || ramp < 0) continue;
      if (e.from !== void 0 && !Number.isFinite(e.from)) continue;
      const t = e.target;
      let target;
      let bus = 0;
      let id;
      if ("effect" in t) {
        if (!this.chainFor(t.effect.bus).some((x) => x.id === t.effect.effect)) continue;
        target = 0;
        bus = t.effect.bus;
        id = t.effect.effect;
      } else if ("instrument" in t) {
        if (!this.instruments.has(t.instrument)) continue;
        target = 1;
        id = t.instrument;
      } else if ("bus" in t) {
        if (!this.knowsBusTarget(t.bus)) continue;
        target = 3;
        bus = t.bus;
        id = 0;
      } else {
        if (!this.samples.has(t.sample)) continue;
        target = 2;
        id = t.sample;
      }
      wire.push({ time: e.time, target, bus, id, param: e.param, value: e.value, from: e.from, ramp });
    }
    if (wire.length > 0) this.bridge.send({ type: "schedule-params", events: wire });
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
      case "meters":
        this.meters = decodeMeters(event.peaks);
        for (const l of this.meterListeners) l(this.meters);
        break;
      case "analysis":
        {
          const frame = { slot: event.slot, samples: event.samples, sampleRate: this.capabilities.sampleRate };
          for (const l of this.analysisListeners) l(frame);
        }
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
  ANALYSIS_TAP_SLOTS,
  AudioRuntime,
  BassParam,
  CLIP_THRESHOLD,
  ClapParam,
  DmChaosParam,
  DmCrackParam,
  DmMetalParam,
  DmThumpParam,
  DmToneParam,
  DubDelayDivision,
  EQ_BAND_COUNT,
  EQ_MAX_GAIN_DB,
  EQ_MAX_HZ,
  EQ_MAX_Q,
  EQ_MAX_SHELF_SLOPE,
  EQ_MIN_HZ,
  EQ_MIN_Q,
  EQ_MIN_SHELF_SLOPE,
  EdgeParam,
  EffectKind,
  EqBandType,
  FilterMode,
  FilterSlope,
  FmParam,
  HOST_PROTOCOL_VERSION,
  HiHatParam,
  HypersawParam,
  InstrumentCommonParam,
  InstrumentKind,
  KickParam,
  LFO_DIVISIONS,
  LfoWaveform,
  MASTER_BUS,
  MAX_EFFECTS_PER_BUS,
  MAX_INSTRUMENTS,
  MAX_SEQUENCE_EVENTS,
  MAX_SLICES,
  MAX_TRACK_BUSES,
  METER_LED_DB,
  METER_MAX_DB,
  METER_MIN_DB,
  METER_RED_DB,
  METER_RED_LEDS,
  MIN_SLICE,
  MeterTracker,
  NodeParam,
  PadParam,
  PercSynthParam,
  PulseParam,
  PwmBeastParam,
  SampleParam,
  SnareParam,
  SpectrumAnalyzer,
  SpectrumFeed,
  TomParam,
  VoiceParam,
  addCue,
  busPeak,
  computePeaks,
  cueRegions,
  dbToUnit,
  decodeMeters,
  defaultEqBands,
  emptyMeters,
  eqBandParam,
  eqBandResponseDb,
  eqBandSections,
  eqBandToParams,
  eqBandUses,
  eqCurve,
  eqResponseDb,
  evenCues,
  getHostFiles,
  getHostState,
  hasEmbeddedHost,
  hostBeatAt,
  hostProtocolWarning,
  hostTimeAt,
  isHostDiscontinuity,
  lfoShape,
  lfoValueAt,
  logFreqToUnit,
  metersLitLeds,
  moveCue,
  normalizeCues,
  normalizeMidiRoute,
  normalizeSequence,
  peakToDb,
  peakToUnit,
  regionForNote,
  removeCue,
  sameMidiRoute,
  sliceCount,
  spectrumPoints
};
//# sourceMappingURL=index.js.map