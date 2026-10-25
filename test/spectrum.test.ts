import { describe, expect, it } from "vitest";
import { SpectrumAnalyzer } from "../src/runtime/spectrum";
import { dbToUnit, logFreqToUnit } from "../src/runtime/scale";

const SR = 48000;
const sine = (hz: number, amp: number, n = 2048) => Float32Array.from({ length: n }, (_, i) => amp * Math.sin((2 * Math.PI * hz * i) / SR));

describe("SpectrumAnalyzer", () => {
  it("peaks at the sine's column at about its level", () => {
    const a = new SpectrumAnalyzer();
    a.push(sine(1000, 0.5), SR, 0.04);
    let best = 0;
    for (let c = 1; c < a.columns; c++) if (a.db[c] > a.db[best]) best = c;
    expect(Math.abs(Math.log2(a.frequencies[best] / 1000))).toBeLessThan(0.1);
    expect(a.db[best]).toBeGreaterThan(-6 - 3);
    expect(a.db[best]).toBeLessThan(-6 + 1); // 0.5 amplitude = -6 dBFS
  });
  it("reads the floor for silence and has no NaN", () => {
    const a = new SpectrumAnalyzer();
    a.push(new Float32Array(2048), SR, 0.04);
    for (const v of a.db) expect(v).toBe(a.floorDb);
  });
  it("falls gradually and rises instantly", () => {
    const a = new SpectrumAnalyzer({ fallDbPerSec: 40 });
    a.push(sine(440, 0.5), SR, 0.04);
    const peak = Math.max(...a.db);
    a.push(new Float32Array(2048), SR, 0.5);
    expect(Math.max(...a.db)).toBeCloseTo(peak - 20, 0);
    a.push(sine(440, 0.5), SR, 0.04);
    expect(Math.max(...a.db)).toBeCloseTo(peak, 0);
    a.decay(10);
    expect(Math.max(...a.db)).toBe(a.floorDb);
  });
  it("uses the newest samples of a longer window and ignores short ones", () => {
    const a = new SpectrumAnalyzer();
    a.push(sine(1000, 0.5, 100), SR, 0.04);
    expect(Math.max(...a.db)).toBe(a.floorDb);
    a.push(sine(1000, 0.5, 4096), SR, 0.04);
    expect(Math.max(...a.db)).toBeGreaterThan(-10);
  });
  it("maps axes", () => {
    expect(logFreqToUnit(20, 20, 20000)).toBeCloseTo(0);
    expect(logFreqToUnit(20000, 20, 20000)).toBeCloseTo(1);
    expect(logFreqToUnit(632.45553, 20, 20000)).toBeCloseTo(0.5);
    expect(dbToUnit(-12, -12, 12)).toBe(0);
    expect(dbToUnit(0, -12, 12)).toBe(0.5);
  });
});

import { SpectrumFeed, spectrumPoints, type AnalysisFrame } from "../src/runtime/spectrum";

describe("SpectrumFeed", () => {
  const mk = (freeSlots = 4) => {
    let cb: ((f: AnalysisFrame) => void) | null = null;
    const calls: Array<[string, ...unknown[]]> = [];
    let free = freeSlots, next = 0;
    const source = {
      openAnalysisTap: (b: number, e: number, post: boolean) => {
        calls.push(["open", b, e, post]);
        return free-- > 0 ? next++ : null;
      },
      closeAnalysisTap: (slot: number) => { calls.push(["close", slot]); },
      onAnalysis: (fn: (f: AnalysisFrame) => void) => { cb = fn; return () => { cb = null; }; },
    };
    return {
      source, calls, subscribed: () => cb !== null,
      emit: (s: Float32Array, slot = 0) => cb?.({ slot, samples: s, sampleRate: SR }),
    };
  };
  it("opens a slot, defers the FFT to frame(), settles when idle, and cleans up", () => {
    const m = mk();
    const a = new SpectrumAnalyzer({ fallDbPerSec: 100 });
    const feed = new SpectrumFeed(m.source, 3, 7, true, a);
    expect(m.calls).toEqual([["open", 3, 7, true]]);
    m.emit(sine(1000, 0.5)); m.emit(sine(1000, 0.5));
    expect(Math.max(...a.db)).toBe(a.floorDb); // nothing computed until a frame
    expect(feed.frame(1000)).toBe(true);
    expect(Math.max(...a.db)).toBeGreaterThan(-10);
    expect(feed.frame(1016)).toBe(true);  // decaying
    for (let t = 1250; t <= 3000; t += 250) feed.frame(t); // reaches the floor
    expect(Math.max(...a.db)).toBe(a.floorDb);
    expect(feed.frame(3016)).toBe(false); // settled: no redraw needed
    feed.dispose();
    expect(m.calls[m.calls.length - 1]).toEqual(["close", 0]);
    expect(m.subscribed()).toBe(false);
  });
  it("shows only its own slot, so a pre and a post feed stay independent", () => {
    const m = mk();
    const pre = new SpectrumAnalyzer(), post = new SpectrumAnalyzer();
    const fPre = new SpectrumFeed(m.source, 3, 7, false, pre);   // slot 0
    const fPost = new SpectrumFeed(m.source, 3, 7, true, post);  // slot 1
    expect(m.calls.filter((c) => c[0] === "open").map((c) => c[3])).toEqual([false, true]);
    m.emit(sine(1000, 0.5), 1);
    fPre.frame(1000); fPost.frame(1000);
    expect(Math.max(...pre.db)).toBe(pre.floorDb);
    expect(Math.max(...post.db)).toBeGreaterThan(-10);
  });
  it("stays empty and harmless when no slot is free", () => {
    const m = mk(0);
    const a = new SpectrumAnalyzer();
    const feed = new SpectrumFeed(m.source, 3, 7, false, a);
    m.emit(sine(1000, 0.5), 0);
    feed.frame(1000);
    expect(Math.max(...a.db)).toBe(a.floorDb);
    feed.dispose();
    expect(m.calls.some((c) => c[0] === "close")).toBe(false);
  });
});

describe("spectrumPoints", () => {
  it("maps columns to x (log) and level to y (clamped, y down)", () => {
    const a = new SpectrumAnalyzer({ columns: 8 });
    a.db.fill(-100); a.db[7] = 10;
    const out = new Float32Array(16);
    spectrumPoints(a, 640, 190, -90, 0, out);
    expect(out[0]).toBeGreaterThan(0); expect(out[14]).toBeLessThan(640);
    expect(out[14]).toBeGreaterThan(out[0]);
    expect(out[1]).toBe(190); // below range -> bottom
    expect(out[15]).toBe(0);  // above range -> top
  });
});

import { METER_MAX_DB, METER_MIN_DB, METER_RED_DB, peakToUnit } from "../src/runtime/meters";
describe("meter scale", () => {
  it("maps a peak onto the shared dB scale", () => {
    expect(peakToUnit(0)).toBe(0);
    expect(peakToUnit(1)).toBeCloseTo(1);
    expect(peakToUnit(2)).toBe(1);
    expect(peakToUnit(Math.pow(10, METER_MIN_DB / 20))).toBeCloseTo(0, 3);
    expect(METER_MAX_DB).toBe(0);
    expect(METER_RED_DB).toBe(-2);
    expect(peakToUnit(Math.pow(10, -12 / 20))).toBeCloseTo(0.75, 2);
  });
});
