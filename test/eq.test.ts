import { describe, expect, it } from "vitest";
import {
  EQ_BAND_COUNT, EqBandType, NodeParam, defaultEqBands, eqBandParam, eqBandToParams, eqBandUses,
  eqCurve, eqResponseDb,
} from "../src/runtime";

const SR = 48000;
const band = (over: Partial<ReturnType<typeof defaultEqBands>[0]>) => ({ ...defaultEqBands()[1], enabled: true, ...over });

// These are the same analytic values native/test/eq_test.cpp measures on the real DSP, so the
// client-side curve and the engine cannot drift apart without one of the two suites failing.
describe("EQ response (client-side curve)", () => {
  it("default bands are disabled and neutral", () => {
    const bands = defaultEqBands();
    expect(bands).toHaveLength(EQ_BAND_COUNT);
    for (const f of [30, 1000, 15000]) expect(eqResponseDb(bands, f, SR)).toBe(0);
  });

  it("bell: full gain at the centre, flat far away, narrower with Q", () => {
    const b = band({ frequency: 1000, gain: 9, q: 2 });
    expect(eqResponseDb([b], 1000, SR)).toBeCloseTo(9, 2);
    expect(Math.abs(eqResponseDb([b], 60, SR))).toBeLessThan(0.3);
    const narrow = eqResponseDb([band({ frequency: 1000, gain: 9, q: 8 })], 2000, SR);
    const wide = eqResponseDb([band({ frequency: 1000, gain: 9, q: 0.5 })], 2000, SR);
    expect(narrow).toBeLessThan(wide - 3);
  });

  it("shelves reach their gain on the plateau and half of it at the corner", () => {
    const low = band({ type: EqBandType.LowShelf, frequency: 200, gain: 12 });
    expect(eqResponseDb([low], 30, SR)).toBeCloseTo(12, 0);
    expect(eqResponseDb([low], 200, SR)).toBeCloseTo(6, 1);
    const high = band({ type: EqBandType.HighShelf, frequency: 4000, gain: -12 });
    expect(eqResponseDb([high], 18000, SR)).toBeCloseTo(-12, 0);
    expect(Math.abs(eqResponseDb([high], 100, SR))).toBeLessThan(0.2);
  });

  it("low/high-pass: -3 dB at the corner, 24 dB/oct twice as steep as 12", () => {
    const lp12 = band({ type: EqBandType.LowPass, frequency: 1000 });
    const lp24 = { ...lp12, slope: 24 as const };
    expect(eqResponseDb([lp12], 1000, SR)).toBeCloseTo(-3.01, 1);
    expect(eqResponseDb([lp24], 1000, SR)).toBeCloseTo(-3.01, 1);
    expect(eqResponseDb([lp24], 4000, SR)).toBeLessThan(eqResponseDb([lp12], 4000, SR) - 15);
    const hp = band({ type: EqBandType.HighPass, frequency: 1000 });
    expect(eqResponseDb([hp], 1000, SR)).toBeCloseTo(-3.01, 1);
    expect(eqResponseDb([hp], 8000, SR)).toBeGreaterThan(-0.1);
  });

  it("notch cuts deeply at its centre only", () => {
    const n = band({ type: EqBandType.Notch, frequency: 1000, q: 4 });
    expect(eqResponseDb([n], 1000, SR)).toBeLessThan(-40);
    expect(Math.abs(eqResponseDb([n], 100, SR))).toBeLessThan(0.1);
  });

  it("bands in series add in dB; a disabled band contributes nothing", () => {
    const a = band({ frequency: 1000, gain: 6, q: 1 });
    const b = band({ frequency: 1000, gain: 5, q: 1 });
    expect(eqResponseDb([a, b], 1000, SR)).toBeCloseTo(11, 1);
    expect(eqResponseDb([a, { ...b, enabled: false }], 1000, SR)).toBeCloseTo(6, 1);
  });

  it("eqCurve samples a log axis with per-band and combined curves", () => {
    const bands = defaultEqBands().map((b, i) => (i === 1 ? { ...b, enabled: true, gain: 6 } : b));
    const c = eqCurve(bands, SR, 64);
    expect(c.frequencies[0]).toBeCloseTo(20, 6);
    expect(c.frequencies[63]).toBeCloseTo(20000, 3);
    expect(c.frequencies[32] / c.frequencies[31]).toBeCloseTo(c.frequencies[1] / c.frequencies[0], 6);
    expect(c.bandDb).toHaveLength(4);
    expect(Math.max(...c.bandDb[1])).toBeGreaterThan(5);
    expect(Math.max(...c.bandDb[0])).toBe(0);
    for (let i = 0; i < 64; i++) expect(c.combinedDb[i]).toBeCloseTo(c.bandDb[1][i], 9);
  });
});

describe("EQ parameter addressing", () => {
  it("maps band fields onto the native NodeParam ids", () => {
    expect(eqBandParam(0, "enabled")).toBe(49);
    expect(eqBandParam(0, "slope")).toBe(55);
    expect(eqBandParam(3, "enabled")).toBe(70);
    expect(eqBandParam(3, "slope")).toBe(NodeParam.EqBand4Slope);
    expect(eqBandToParams(1, defaultEqBands()[1]).map(([p]) => p)).toEqual([56, 57, 58, 59, 60, 61, 62]);
  });

  it("reports which fields a type reads", () => {
    expect(eqBandUses(EqBandType.Bell, "gain")).toBe(true);
    expect(eqBandUses(EqBandType.Bell, "slope")).toBe(false);
    expect(eqBandUses(EqBandType.LowShelf, "shelfSlope")).toBe(true);
    expect(eqBandUses(EqBandType.LowShelf, "q")).toBe(false);
    expect(eqBandUses(EqBandType.HighPass, "gain")).toBe(false);
    expect(eqBandUses(EqBandType.HighPass, "slope")).toBe(true);
    expect(eqBandUses(EqBandType.Notch, "q")).toBe(true);
    expect(eqBandUses(EqBandType.Notch, "gain")).toBe(false);
  });
});
