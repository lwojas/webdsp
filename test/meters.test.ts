import { describe, expect, it } from "vitest";
import {
  METER_LED_DB, METER_RED_LEDS, MeterTracker, busPeak, decodeMeters, emptyMeters, metersLitLeds,
} from "../src/runtime/meters";

function snap(masterIn: number, buses: Record<number, number> = {}, gr = 0) {
  const s = emptyMeters(64);
  s.masterIn = masterIn;
  s.masterOut = Math.min(masterIn, 0.97);
  s.gainReductionDb = gr;
  for (const [b, v] of Object.entries(buses)) s.buses[Number(b) - 1] = v;
  return s;
}

describe("meter snapshot decoding", () => {
  it("maps the engine layout", () => {
    const raw = new Float32Array(3 + 64);
    raw[0] = 1.2; raw[1] = 0.97; raw[2] = -3; raw[3 + 4] = 0.5;
    const s = decodeMeters(raw);
    expect(s.masterIn).toBeCloseTo(1.2);
    expect(s.masterOut).toBeCloseTo(0.97);
    expect(s.gainReductionDb).toBe(-3);
    expect(busPeak(s, 5)).toBe(0.5);
    expect(busPeak(s, 0)).toBe(0); // master is not a track bus
    expect(busPeak(s, 999)).toBe(0);
  });
});

describe("LED mapping", () => {
  it("is dark when quiet and rises with level", () => {
    expect(metersLitLeds(0)).toBe(0);
    expect(metersLitLeds(0.001)).toBe(0); // -60 dB
    let last = -1;
    for (const p of [0.01, 0.05, 0.1, 0.25, 0.5, 0.8, 0.95, 1.0]) {
      const n = metersLitLeds(p);
      expect(n).toBeGreaterThanOrEqual(last);
      last = n;
    }
  });
  it("lights the top (clip) LED only at 0 dBFS", () => {
    expect(metersLitLeds(0.99)).toBe(METER_LED_DB.length - 1);
    expect(metersLitLeds(1.0)).toBe(METER_LED_DB.length);
    expect(METER_RED_LEDS).toBeLessThan(METER_LED_DB.length);
  });
});

describe("MeterTracker", () => {
  it("holds a track clip briefly, then clears it, per track", () => {
    const t = new MeterTracker(64, { clipHoldMs: 1000 });
    t.update(snap(0.5, { 3: 1.3, 4: 0.5 }), 0);
    expect(t.busClipping(3, 10)).toBe(true);
    expect(t.busClipping(4, 10)).toBe(false);
    t.update(snap(0.5), 500);
    expect(t.busClipping(3, 500)).toBe(true); // still within the hold
    expect(t.busClipping(3, 1001)).toBe(false); // forgotten: not permanently red
  });
  it("re-arms the hold on repeated clipping", () => {
    const t = new MeterTracker(64, { clipHoldMs: 1000 });
    t.update(snap(0.1, { 1: 1 }), 0);
    t.update(snap(0.1, { 1: 1 }), 900);
    expect(t.busClipping(1, 1800)).toBe(true);
  });
  it("flags master clipping from the pre-limiter peak", () => {
    const t = new MeterTracker(64, { clipHoldMs: 1000 });
    t.update(snap(0.9), 0);
    expect(t.masterClipping(0)).toBe(false);
    t.update(snap(1.4, {}, -3), 20);
    expect(t.masterClipping(20)).toBe(true);
    expect(t.masterGainReductionDb()).toBe(-3);
    expect(t.masterClipping(1100)).toBe(false);
  });
  it("jumps up instantly and falls smoothly", () => {
    const t = new MeterTracker(64, { fallDbPerSec: 20 });
    t.update(snap(1.0), 0);
    expect(t.masterLevel()).toBe(1);
    t.update(snap(0), 1000); // 1 s later at 20 dB/s -> 0.1
    expect(t.masterLevel()).toBeCloseTo(0.1, 3);
    t.update(snap(0), 10000);
    expect(t.masterLevel()).toBe(0);
    expect(t.masterLitLeds()).toBe(0);
  });
});
