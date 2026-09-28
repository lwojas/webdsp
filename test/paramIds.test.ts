import { describe, expect, it } from "vitest";
import { NodeParam, VoiceParam } from "../src/runtime/types";

// Pins the literal numeric values that must match native/src/params.h. There's no
// automatic cross-language check (see the comment on these enums in types.ts), so this is
// what catches an accidental reorder turning into a silent ABI mismatch.
describe("parameter id contract with native/src/params.h", () => {
  it("VoiceParam matches the native enum", () => {
    expect(VoiceParam.Gain).toBe(0);
    expect(VoiceParam.Rate).toBe(1);
    expect(VoiceParam.FilterCutoff).toBe(2);
    expect(VoiceParam.FilterResonance).toBe(3);
  });

  it("NodeParam matches the native enum", () => {
    expect(NodeParam.DelayTime).toBe(0);
    expect(NodeParam.DelayFeedback).toBe(1);
    expect(NodeParam.DelayMix).toBe(2);
    expect(NodeParam.BusGain).toBe(3);
  });
});
