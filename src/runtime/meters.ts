import { dbToUnit } from "./scale";
import type { BusId } from "./types";

// Level metering. The engine measures peaks (native/src/engine.h, readMeters()); this module
// holds the plain-data shape they cross the worklet boundary in, plus the small amount of
// display logic every client would otherwise re-implement: LED thresholds and a hold/decay
// tracker. It never touches audio samples.

/** Peaks since the previous snapshot. Linear, 1.0 = 0 dBFS. */
export interface MeterSnapshot {
  /** Master mix peak *before* the limiter: the level being fed to it. >= 1 means the
   * limiter had to take it down (without a limiter this would have clipped). */
  masterIn: number;
  /** Final output peak, after the limiter. */
  masterOut: number;
  /** Deepest limiter gain reduction in the interval, dB (<= 0). */
  gainReductionDb: number;
  /** Peak of each track bus after its FX chain, gain and pan; index = busId - 1. */
  buses: Float32Array;
}

/** A peak at or above this linear level counts as clipping (0 dBFS). */
export const CLIP_THRESHOLD = 1.0;

/** Layout of the engine's meter array; mirrors Engine::kMeter* in native/src/engine.h. */
export function decodeMeters(raw: Float32Array): MeterSnapshot {
  return {
    masterIn: raw[0],
    masterOut: raw[1],
    gainReductionDb: raw[2],
    buses: raw.subarray(3),
  };
}

export function emptyMeters(maxBuses: number): MeterSnapshot {
  return { masterIn: 0, masterOut: 0, gainReductionDb: 0, buses: new Float32Array(maxBuses) };
}

/** Peak of one track bus in a snapshot (0 for an unknown bus, including MASTER_BUS). */
export function busPeak(snapshot: MeterSnapshot, bus: BusId): number {
  return bus >= 1 && bus <= snapshot.buses.length ? snapshot.buses[bus - 1] : 0;
}

/** Thresholds (dBFS) of the master meter LEDs, lowest first. The top `METER_RED_LEDS` are the
 * danger region; the last sits at 0 dBFS, so it only lights on actual clipping. */
export const METER_LED_DB: readonly number[] = [-48, -36, -24, -18, -12, -8, -4, -2, -0.5, 0];
export const METER_RED_LEDS = 3;

/** The dB range a level meter displays, and where its danger (red) zone starts. Shared by the LED
 * thresholds above and by bar-style meters, so every meter agrees on the scale. */
export const METER_MIN_DB = METER_LED_DB[0];
export const METER_MAX_DB = 0;
export const METER_RED_DB = METER_LED_DB[METER_LED_DB.length - METER_RED_LEDS];

/** Where a linear peak sits on the meter scale, 0 (at/below METER_MIN_DB) to 1 (0 dBFS or above). */
export function peakToUnit(peak: number): number {
  return Math.min(1, Math.max(0, dbToUnit(peakToDb(peak), METER_MIN_DB, METER_MAX_DB)));
}

export function peakToDb(peak: number): number {
  return peak > 1e-6 ? 20 * Math.log10(peak) : -120;
}

/** How many LEDs (from the bottom) are lit for a linear peak. */
export function metersLitLeds(peak: number): number {
  // A hair of tolerance so a peak of exactly 1.0 in float32 lights the clip LED.
  const db = peakToDb(peak) + 1e-4;
  let n = 0;
  while (n < METER_LED_DB.length && db >= METER_LED_DB[n]) n++;
  return n;
}

export interface MeterTrackerOptions {
  /** How long a clip indication stays lit after the last clipping peak. Default 1500 ms. */
  clipHoldMs?: number;
  /** Fall speed of the master level, dB per second. Default 24. */
  fallDbPerSec?: number;
}

/**
 * Turns the stream of peak snapshots into stable display state: a master level that falls
 * smoothly, and clip flags that stay lit for a short hold after the last clipping peak (so a
 * single-block overshoot is visible), then clear on their own. No permanent peak-hold: a clip
 * is forgotten after `clipHoldMs` without further clipping. Time is passed in, so it is pure
 * and testable.
 */
export class MeterTracker {
  private readonly clipHoldMs: number;
  private readonly fallDbPerSec: number;
  private level = 0;
  private masterClipAt = -Infinity;
  private gainReductionDb = 0;
  private lastMs: number | null = null;
  private readonly busClipAt: Float64Array;

  constructor(maxBuses: number, options: MeterTrackerOptions = {}) {
    this.clipHoldMs = options.clipHoldMs ?? 1500;
    this.fallDbPerSec = options.fallDbPerSec ?? 24;
    this.busClipAt = new Float64Array(maxBuses).fill(-Infinity);
  }

  update(snapshot: MeterSnapshot, nowMs: number): void {
    const dt = this.lastMs === null ? 0 : Math.max(0, nowMs - this.lastMs) / 1000;
    this.lastMs = nowMs;
    const decayed = this.level * Math.pow(10, (-this.fallDbPerSec * dt) / 20);
    this.level = Math.max(snapshot.masterIn, decayed < 1e-4 ? 0 : decayed);
    this.gainReductionDb = snapshot.gainReductionDb;
    if (snapshot.masterIn >= CLIP_THRESHOLD) this.masterClipAt = nowMs;
    const n = Math.min(this.busClipAt.length, snapshot.buses.length);
    for (let i = 0; i < n; i++) {
      if (snapshot.buses[i] >= CLIP_THRESHOLD) this.busClipAt[i] = nowMs;
    }
  }

  /** Master level (linear peak, pre-limiter) with fall-off applied. */
  masterLevel(): number {
    return this.level;
  }

  masterLitLeds(): number {
    return metersLitLeds(this.level);
  }

  /** Limiter gain reduction in the latest interval, dB (<= 0). */
  masterGainReductionDb(): number {
    return this.gainReductionDb;
  }

  masterClipping(nowMs: number): boolean {
    return nowMs - this.masterClipAt < this.clipHoldMs;
  }

  busClipping(bus: BusId, nowMs: number): boolean {
    return bus >= 1 && bus <= this.busClipAt.length && nowMs - this.busClipAt[bus - 1] < this.clipHoldMs;
  }

  reset(): void {
    this.level = 0;
    this.masterClipAt = -Infinity;
    this.gainReductionDb = 0;
    this.lastMs = null;
    this.busClipAt.fill(-Infinity);
  }
}
