import { describe, expect, it } from "vitest";
import { LfoWaveform, BassParam, FmParam, HypersawParam, PadParam, PulseParam, PwmBeastParam, EffectKind, FilterMode, FilterSlope, ClapParam, HiHatParam, InstrumentKind, KickParam, SnareParam, TomParam, MAX_EFFECTS_PER_BUS, NodeParam, SampleParam, VoiceParam } from "../src/runtime/types";

// Pins the literal numeric values that must match native/src/params.h. There's no
// automatic cross-language check (see the comment on these enums in types.ts), so this is
// what catches an accidental reorder turning into a silent ABI mismatch.
describe("parameter id contract with native/src/params.h", () => {
  it("VoiceParam matches the native enum", () => {
    expect(VoiceParam.Gain).toBe(0);
    expect(VoiceParam.Rate).toBe(1);
    expect(VoiceParam.FilterCutoff).toBe(2);
    expect(VoiceParam.FilterResonance).toBe(3);
    expect(VoiceParam.FilterMode).toBe(4);
    expect(VoiceParam.FilterSlope).toBe(5);
    expect(VoiceParam.Pan).toBe(6);
  });

  it("NodeParam matches the native enum", () => {
    expect(NodeParam.DelayTime).toBe(0);
    expect(NodeParam.DelayFeedback).toBe(1);
    expect(NodeParam.DelayMix).toBe(2);
    expect(NodeParam.BusGain).toBe(3);
    expect(NodeParam.FilterCutoff).toBe(4);
    expect(NodeParam.FilterResonance).toBe(5);
    expect(NodeParam.FilterMode).toBe(6);
    expect(NodeParam.ReverbDecay).toBe(7);
    expect(NodeParam.ReverbDamping).toBe(8);
    expect(NodeParam.ReverbMix).toBe(9);
    expect(NodeParam.CompThreshold).toBe(10);
    expect(NodeParam.CompRatio).toBe(11);
    expect(NodeParam.CompAttack).toBe(12);
    expect(NodeParam.CompRelease).toBe(13);
    expect(NodeParam.CompKnee).toBe(14);
    expect(NodeParam.CompMakeup).toBe(15);
    expect(NodeParam.SatDrive).toBe(16);
    expect(NodeParam.SatAsymmetry).toBe(17);
    expect(NodeParam.SatOutputGain).toBe(18);
    expect(NodeParam.SatMix).toBe(19);
    expect(NodeParam.ChorusFlangerRate).toBe(20);
    expect(NodeParam.ChorusFlangerDepth).toBe(21);
    expect(NodeParam.ChorusFlangerDelay).toBe(22);
    expect(NodeParam.ChorusFlangerFeedback).toBe(23);
    expect(NodeParam.ChorusFlangerStereoPhase).toBe(24);
    expect(NodeParam.ChorusFlangerMix).toBe(25);
    expect(NodeParam.FilterSlope).toBe(26);
    expect(NodeParam.BusPan).toBe(27);
  });

  it("FilterMode matches the native enum", () => {
    expect(FilterMode.LowPass).toBe(0);
    expect(FilterMode.HighPass).toBe(1);
  });

  it("FilterSlope values are dB/oct", () => {
    expect(FilterSlope.Db12).toBe(12);
    expect(FilterSlope.Db24).toBe(24);
  });

  it("SampleParam matches the native enum", () => {
    expect(SampleParam.Gain).toBe(0);
    expect(SampleParam.EnvAttack).toBe(1);
    expect(SampleParam.EnvDecay).toBe(2);
    expect(SampleParam.EnvSustain).toBe(3);
    expect(SampleParam.EnvRelease).toBe(4);
    expect(SampleParam.Pan).toBe(5);
  });

  it("EffectKind matches native FxKind and MAX_EFFECTS_PER_BUS matches kMaxFxPerBus", () => {
    expect(EffectKind.Filter).toBe(0);
    expect(EffectKind.ChorusFlanger).toBe(1);
    expect(EffectKind.Delay).toBe(2);
    expect(EffectKind.Reverb).toBe(3);
    expect(EffectKind.Compressor).toBe(4);
    expect(EffectKind.Saturation).toBe(5);
    expect(EffectKind.Phaser).toBe(6);
    expect(EffectKind.Bitcrusher).toBe(7);
    expect(NodeParam.PhaserRate).toBe(28);
    expect(NodeParam.PhaserMix).toBe(33);
    expect(NodeParam.CrushBits).toBe(34);
    expect(NodeParam.CrushMix).toBe(36);
    expect(MAX_EFFECTS_PER_BUS).toBe(16);
  });
});

describe("instrument id contract with native/src/params.h", () => {
  it("InstrumentKind and KickParam match the native enums", () => {
    expect(InstrumentKind.Kick).toBe(0);
    expect(KickParam.Tune).toBe(0);
    expect(KickParam.PitchDrop).toBe(1);
    expect(KickParam.PitchDecay).toBe(2);
    expect(KickParam.Decay).toBe(3);
    expect(KickParam.Punch).toBe(4);
    expect(KickParam.Drive).toBe(5);
    expect([InstrumentKind.Snare, InstrumentKind.Clap, InstrumentKind.Tom, InstrumentKind.HiHat]).toEqual([1, 2, 3, 4]);
    expect([SnareParam.Tune, SnareParam.Decay, SnareParam.Snap, SnareParam.Noise]).toEqual([0, 1, 2, 3]);
    expect([ClapParam.Tune, ClapParam.Decay, ClapParam.Spread, ClapParam.Snap]).toEqual([0, 1, 2, 3]);
    expect([TomParam.Tune, TomParam.PitchDrop, TomParam.Decay, TomParam.Body, TomParam.Damp]).toEqual([0, 1, 2, 3, 4]);
    expect([HiHatParam.Tune, HiHatParam.Decay, HiHatParam.Metal, HiHatParam.Tone]).toEqual([0, 1, 2, 3]);
    expect(InstrumentKind.Bass).toBe(5);
    expect([
      BassParam.Tune, BassParam.Body, BassParam.Bite, BassParam.Cutoff, BassParam.Resonance,
      BassParam.Attack, BassParam.Decay, BassParam.Sustain, BassParam.Release,
      BassParam.Legato, BassParam.Slide,
    ]).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    expect(InstrumentKind.Pulse).toBe(6);
    expect([
      PulseParam.Tune, PulseParam.Cutoff, PulseParam.Resonance, PulseParam.Pump, PulseParam.Repeat,
      PulseParam.Rate, PulseParam.Diffuse, PulseParam.Attack, PulseParam.Release,
    ]).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8]);
    expect(InstrumentKind.Pad).toBe(7);
    expect([
      PadParam.Tune, PadParam.Tone, PadParam.Movement, PadParam.Detune, PadParam.Space,
      PadParam.Attack, PadParam.Release,
    ]).toEqual([0, 1, 2, 3, 4, 5, 6]);
    expect(InstrumentKind.Fm).toBe(8);
    expect([
      FmParam.Tune, FmParam.Ratio, FmParam.Timbre, FmParam.Snap, FmParam.Tone, FmParam.Attack,
      FmParam.Decay, FmParam.Sustain, FmParam.Release, FmParam.Legato, FmParam.Slide,
    ]).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    expect(InstrumentKind.Hypersaw).toBe(9);
    expect([
      HypersawParam.Tune, HypersawParam.Detune, HypersawParam.Spread, HypersawParam.Density,
      HypersawParam.Cutoff, HypersawParam.Attack, HypersawParam.Decay, HypersawParam.Sustain,
      HypersawParam.Release,
    ]).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8]);
    expect(InstrumentKind.PwmBeast).toBe(10);
    expect([
      PwmBeastParam.Tune, PwmBeastParam.Width, PwmBeastParam.Pwm, PwmBeastParam.Rate,
      PwmBeastParam.Shape, PwmBeastParam.Harmonics, PwmBeastParam.Drive, PwmBeastParam.Cutoff,
      PwmBeastParam.Resonance, PwmBeastParam.EnvAmount, PwmBeastParam.Sub, PwmBeastParam.Attack,
      PwmBeastParam.Decay, PwmBeastParam.Sustain, PwmBeastParam.Release,
    ]).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]);
  });
});

describe("LFO waveform ids match native/src/dsp/lfo.h", () => {
  it("pins LfoWaveform", () => {
    expect([LfoWaveform.Sine, LfoWaveform.Triangle, LfoWaveform.Square, LfoWaveform.Saw]).toEqual([0, 1, 2, 3]);
  });
});
