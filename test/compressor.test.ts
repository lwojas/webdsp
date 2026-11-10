import { describe, expect, it } from "vitest";
import {
  COMPRESSOR_PRESETS,
  GainReductionHistory,
  LoudnessChange,
  compressorCurveDb,
  presetSettings,
  reductionCharacter,
  staticGainReductionDb,
} from "../src/runtime/compressor";

describe("compressor curve", () => {
  const s = { threshold: -20, ratio: 4, knee: 0 };
  it("is 1:1 under the threshold and ratio-sloped above it", () => {
    expect(compressorCurveDb(-40, s)).toBe(-40);
    expect(compressorCurveDb(-20, s)).toBeCloseTo(-20);
    expect(compressorCurveDb(0, s)).toBeCloseTo(-15); // 20 dB over at 4:1 -> 5 dB over
    expect(staticGainReductionDb(0, { ...s, ratio: 10 })).toBeCloseTo(-18);
  });
  it("matches the engine's soft knee: -(1 - 1/R) * knee / 8 at the threshold", () => {
    expect(staticGainReductionDb(-20, { ...s, knee: 12 })).toBeCloseTo(-(0.75 * 12) / 8);
    expect(staticGainReductionDb(-30, { ...s, knee: 12 })).toBe(0);
  });
  it("ratio 1 never reduces", () => {
    expect(staticGainReductionDb(-3, { threshold: -30, ratio: 1, knee: 6 })).toBeCloseTo(0);
  });
});

describe("presets", () => {
  it("sit below the peak by their calibrated distance and use explicit makeup", () => {
    for (const p of COMPRESSOR_PRESETS) {
      const s = presetSettings(p, -3);
      expect(s.threshold).toBeCloseTo(-3 - p.thresholdBelowPeakDb, 0);
      expect(s.makeup).toBe(p.makeup);
      expect(s.ratio).toBe(p.ratio);
    }
    expect(COMPRESSOR_PRESETS.map((p) => p.name)).toEqual(["Drum Glue", "Punch", "Tight", "Smash"]);
  });
  it("follow the level they are fitted to", () => {
    const hot = presetSettings(COMPRESSOR_PRESETS[1], -1).threshold;
    const quiet = presetSettings(COMPRESSOR_PRESETS[1], -13).threshold;
    expect(hot - quiet).toBeCloseTo(12, 0);
  });
  it("are ordered by intended reduction", () => {
    const t = COMPRESSOR_PRESETS.map((p) => p.targetGrDb);
    expect([...t].sort((a, b) => a - b)).toEqual(t);
  });
});

describe("level matching and display helpers", () => {
  it("tracks the energy-weighted loudness change", () => {
    const l = new LoudnessChange();
    expect(l.db()).toBe(0);
    l.push(0.5, -6, 2); // loud: net -4
    l.push(0.01, 0, 2); // near silence barely counts
    expect(l.db()).toBeCloseTo(-4, 0);
    l.reset();
    expect(l.db()).toBe(0);
  });
  it("keeps the newest history last", () => {
    const h = new GainReductionHistory(4);
    for (const v of [-1, -2, -3, -4, -5]) h.push(v);
    expect(Array.from(h.values())).toEqual([-2, -3, -4, -5]);
  });
  it("names the amount of compression", () => {
    expect([0, -1, -3, -6, -10, -15].map(reductionCharacter)).toEqual(["idle", "light", "moderate", "firm", "heavy", "extreme"]);
  });
});
