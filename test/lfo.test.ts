import { describe, expect, it } from "vitest";
import { LFO_DIVISIONS, lfoShape, lfoValueAt } from "../src/runtime/lfo";
import { LfoWaveform } from "../src/runtime/types";

describe("lfo (event-time evaluation)", () => {
  it("has the engine's waveforms", () => {
    expect(lfoShape(LfoWaveform.Sine, 0.25)).toBeCloseTo(1);
    expect(lfoShape(LfoWaveform.Triangle, 0.75)).toBeCloseTo(-1);
    expect(lfoShape(LfoWaveform.Square, 0.49)).toBe(1);
    expect(lfoShape(LfoWaveform.Square, 0.5)).toBe(-1);
    expect(lfoShape(LfoWaveform.Saw, 0.5)).toBeCloseTo(0);
    expect(lfoShape(LfoWaveform.Saw, -0.5)).toBeCloseTo(0);
  });

  it("free mode: rate in Hz from the anchor, deterministic", () => {
    const s = { waveform: LfoWaveform.Saw, sync: false, rateHz: 2, division: 0 };
    expect(lfoValueAt(s, { time: 10, anchorTime: 10, bpm: 120 })).toBeCloseTo(-1);
    expect(lfoValueAt(s, { time: 10.125, anchorTime: 10, bpm: 120 })).toBeCloseTo(-0.5);
    const ctx = { time: 13.37, anchorTime: 10, bpm: 97 };
    expect(lfoValueAt(s, ctx)).toBe(lfoValueAt(s, ctx));
  });

  it("sync mode: period from bpm and division; independent of rateHz", () => {
    const quarter = LFO_DIVISIONS.findIndex((d) => d.name === "1/4");
    const s = { waveform: LfoWaveform.Saw, sync: true, rateHz: 99, division: quarter };
    expect(lfoValueAt(s, { time: 1.25, anchorTime: 1, bpm: 120 })).toBeCloseTo(0); // half of a 0.5 s cycle
    expect(lfoValueAt(s, { time: 1.5, anchorTime: 1, bpm: 120 })).toBeCloseTo(-1);
    expect(lfoValueAt(s, { time: 1.5, anchorTime: 1, bpm: 60 })).toBeCloseTo(0); // 60 BPM: 1 s cycle
  });

  it("phase offset shifts the cycle", () => {
    const s = { waveform: LfoWaveform.Sine, sync: false, rateHz: 1, division: 0, phase: 0.25 };
    expect(lfoValueAt(s, { time: 0, anchorTime: 0, bpm: 120 })).toBeCloseTo(1);
  });
});
