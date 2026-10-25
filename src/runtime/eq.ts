import { EqBandType, NodeParam } from "./types";

// Pure, UI-free description of the EQ effect (EffectKind.Eq) for clients: the band model, how a
// band maps onto NodeParam ids, and the frequency response a set of bands produces, so an
// application can draw the curve (and a handle per band) without asking the engine. It mirrors
// the coefficient maths of native `dsp/eq.h` (RBJ Audio EQ Cookbook, same frequency clamp, same
// 24 dB/oct Butterworth split); test/eq.test.ts pins both ends to the same analytic values. The
// engine does not use this file; it is not required to run the effect. See docs/eq.md.

export const EQ_BAND_COUNT = 4;
export const EQ_MIN_HZ = 20;
export const EQ_MAX_HZ = 20000;
export const EQ_MAX_GAIN_DB = 18;
export const EQ_MIN_Q = 0.1;
export const EQ_MAX_Q = 18;
export const EQ_MIN_SHELF_SLOPE = 0.1;
export const EQ_MAX_SHELF_SLOPE = 1;

/** One band, in the units of the NodeParams (frequency Hz, gain dB). Fields a type does not use
 * (see `eqBandUses`) are kept and ignored, so changing a band's type does not lose them. */
export interface EqBand {
  enabled: boolean;
  type: EqBandType;
  frequency: number;
  gain: number;
  q: number;
  /** Shelf slope S (0.1..1, 1 = steepest without overshoot). */
  shelfSlope: number;
  /** Low-pass / high-pass steepness in dB/oct: 12 or 24. */
  slope: 12 | 24;
}

/** The engine's default bands (what a freshly added EQ holds): all disabled, so it is neutral. */
export function defaultEqBands(): EqBand[] {
  const mk = (type: EqBandType, frequency: number): EqBand => ({
    enabled: false, type, frequency, gain: 0, q: 0.707, shelfSlope: 1, slope: 12,
  });
  return [
    mk(EqBandType.LowShelf, 100),
    mk(EqBandType.Bell, 500),
    mk(EqBandType.Bell, 2000),
    mk(EqBandType.HighShelf, 8000),
  ];
}

export type EqBandField = "enabled" | "type" | "frequency" | "gain" | "q" | "shelfSlope" | "slope";
const FIELD_OFFSET: Record<EqBandField, number> = {
  enabled: 0, type: 1, frequency: 2, gain: 3, q: 4, shelfSlope: 5, slope: 6,
};

/** NodeParam id of one band field (`band` 0..3). Pass it to `setEffectParameter`. */
export function eqBandParam(band: number, field: EqBandField): NodeParam {
  return NodeParam.EqBand1On + band * 7 + FIELD_OFFSET[field];
}

/** Whether a band type reads a field, for showing only meaningful controls/handles. Frequency,
 * enabled and type are always used. */
export function eqBandUses(type: EqBandType, field: EqBandField): boolean {
  switch (field) {
    case "gain": return type === EqBandType.Bell || type === EqBandType.LowShelf || type === EqBandType.HighShelf;
    case "q": return type === EqBandType.Bell || type === EqBandType.Notch;
    case "shelfSlope": return type === EqBandType.LowShelf || type === EqBandType.HighShelf;
    case "slope": return type === EqBandType.LowPass || type === EqBandType.HighPass;
    default: return true;
  }
}

/** `[param, value]` pairs that put one band into the given state (all seven fields). */
export function eqBandToParams(band: number, b: EqBand): [NodeParam, number][] {
  return [
    [eqBandParam(band, "enabled"), b.enabled ? 1 : 0],
    [eqBandParam(band, "type"), b.type],
    [eqBandParam(band, "frequency"), b.frequency],
    [eqBandParam(band, "gain"), b.gain],
    [eqBandParam(band, "q"), b.q],
    [eqBandParam(band, "shelfSlope"), b.shelfSlope],
    [eqBandParam(band, "slope"), b.slope],
  ];
}

interface Biquad { b0: number; b1: number; b2: number; a1: number; a2: number }

const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

function rbj(type: EqBandType, omega: number, q: number, gainDb: number, s: number): Biquad {
  const sinO = Math.sin(omega), cosO = Math.cos(omega);
  const A = Math.pow(10, gainDb / 40);
  let b0: number, b1: number, b2: number, a0: number, a1: number, a2: number;
  switch (type) {
    case EqBandType.Bell: {
      const alpha = sinO / (2 * q);
      b0 = 1 + alpha * A; b1 = -2 * cosO; b2 = 1 - alpha * A;
      a0 = 1 + alpha / A; a1 = -2 * cosO; a2 = 1 - alpha / A;
      break;
    }
    case EqBandType.LowShelf:
    case EqBandType.HighShelf: {
      const t = 2 * Math.sqrt(A) * (sinO / 2) * Math.sqrt((A + 1 / A) * (1 / s - 1) + 2);
      if (type === EqBandType.LowShelf) {
        b0 = A * ((A + 1) - (A - 1) * cosO + t);
        b1 = 2 * A * ((A - 1) - (A + 1) * cosO);
        b2 = A * ((A + 1) - (A - 1) * cosO - t);
        a0 = (A + 1) + (A - 1) * cosO + t;
        a1 = -2 * ((A - 1) + (A + 1) * cosO);
        a2 = (A + 1) + (A - 1) * cosO - t;
      } else {
        b0 = A * ((A + 1) + (A - 1) * cosO + t);
        b1 = -2 * A * ((A - 1) + (A + 1) * cosO);
        b2 = A * ((A + 1) + (A - 1) * cosO - t);
        a0 = (A + 1) - (A - 1) * cosO + t;
        a1 = 2 * ((A - 1) - (A + 1) * cosO);
        a2 = (A + 1) - (A - 1) * cosO - t;
      }
      break;
    }
    case EqBandType.LowPass: {
      const alpha = sinO / (2 * q);
      b0 = (1 - cosO) / 2; b1 = 1 - cosO; b2 = b0;
      a0 = 1 + alpha; a1 = -2 * cosO; a2 = 1 - alpha;
      break;
    }
    case EqBandType.HighPass: {
      const alpha = sinO / (2 * q);
      b0 = (1 + cosO) / 2; b1 = -(1 + cosO); b2 = b0;
      a0 = 1 + alpha; a1 = -2 * cosO; a2 = 1 - alpha;
      break;
    }
    default: { // Notch
      const alpha = sinO / (2 * q);
      b0 = 1; b1 = -2 * cosO; b2 = 1;
      a0 = 1 + alpha; a1 = -2 * cosO; a2 = 1 - alpha;
    }
  }
  return { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 };
}

/** The biquad section(s) the engine runs for a band: two for a 24 dB/oct low/high-pass, else one.
 * Values are clamped to the engine's ranges first, as the engine does. */
export function eqBandSections(b: EqBand, sampleRate: number): Biquad[] {
  const freq = Math.min(clamp(b.frequency, EQ_MIN_HZ, EQ_MAX_HZ), sampleRate * 0.49);
  const omega = (2 * Math.PI * freq) / sampleRate;
  const pass = b.type === EqBandType.LowPass || b.type === EqBandType.HighPass;
  if (pass && b.slope >= 18) {
    return [rbj(b.type, omega, 0.5411961, 0, 1), rbj(b.type, omega, 1.30656296, 0, 1)];
  }
  return [rbj(b.type, omega, pass ? 0.70710678 : clamp(b.q, EQ_MIN_Q, EQ_MAX_Q),
    clamp(b.gain, -EQ_MAX_GAIN_DB, EQ_MAX_GAIN_DB), clamp(b.shelfSlope, EQ_MIN_SHELF_SLOPE, EQ_MAX_SHELF_SLOPE))];
}

function magnitudeDb(k: Biquad, omega: number): number {
  // |H(e^jw)|^2 for H = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
  const c1 = Math.cos(omega), c2 = Math.cos(2 * omega), s1 = Math.sin(omega), s2 = Math.sin(2 * omega);
  const nr = k.b0 + k.b1 * c1 + k.b2 * c2, ni = -(k.b1 * s1 + k.b2 * s2);
  const dr = 1 + k.a1 * c1 + k.a2 * c2, di = -(k.a1 * s1 + k.a2 * s2);
  return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

/** Response of ONE band at `frequency` Hz, in dB. 0 for a disabled band. */
export function eqBandResponseDb(b: EqBand, frequency: number, sampleRate: number): number {
  if (!b.enabled) return 0;
  const omega = (2 * Math.PI * frequency) / sampleRate;
  let db = 0;
  for (const k of eqBandSections(b, sampleRate)) db += magnitudeDb(k, omega);
  return db;
}

/** Combined response of the series chain at `frequency` Hz, in dB (the bands' dB values add). */
export function eqResponseDb(bands: readonly EqBand[], frequency: number, sampleRate: number): number {
  let db = 0;
  for (const b of bands) db += eqBandResponseDb(b, frequency, sampleRate);
  return db;
}

export interface EqCurve {
  /** Log-spaced frequencies, Hz. */
  frequencies: Float64Array;
  /** Combined response per frequency, dB. */
  combinedDb: Float64Array;
  /** Per-band response per frequency, dB (0 where the band is disabled); one array per band. */
  bandDb: Float64Array[];
}

/** Samples the combined and per-band responses on a log frequency axis, ready to plot. */
export function eqCurve(
  bands: readonly EqBand[], sampleRate: number, points = 256, minHz = EQ_MIN_HZ, maxHz = EQ_MAX_HZ,
): EqCurve {
  const frequencies = new Float64Array(points);
  const combinedDb = new Float64Array(points);
  const bandDb = bands.map(() => new Float64Array(points));
  const ratio = Math.log(maxHz / minHz);
  for (let i = 0; i < points; i++) {
    const f = minHz * Math.exp((ratio * i) / Math.max(1, points - 1));
    frequencies[i] = f;
    bands.forEach((b, j) => {
      const db = eqBandResponseDb(b, f, sampleRate);
      bandDb[j][i] = db;
      combinedDb[i] += db;
    });
  }
  return { frequencies, combinedDb, bandDb };
}
