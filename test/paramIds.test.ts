import { describe, expect, it } from "vitest";
import { FilterMode, NodeParam, VoiceParam } from "../src/runtime/types";

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
  });

  it("FilterMode matches the native enum", () => {
    expect(FilterMode.LowPass).toBe(0);
    expect(FilterMode.HighPass).toBe(1);
  });
});
