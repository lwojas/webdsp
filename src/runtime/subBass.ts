// Pure, UI-free description of the Sub Bass oscillator's harmonic content (InstrumentKind.SubBass), so a client can
// draw the waveform and spectrum a Shape/Profile produces without asking the engine. It mirrors native
// `dsp/harmonic_osc.h` (`harmonicSpectrum`); test/subBass.test.ts and native/test/subbass_test.cpp pin both ends to the
// same numbers. The engine does not use this file. See docs/subbass-instrument.md and docs/visualisation.md.

export const HARMONIC_MAX = 64;
/** Highest harmonic stays below this fraction of the sample rate (native `kHarmonicBandLimit`). */
export const HARMONIC_BAND_LIMIT = 0.42;

interface ProfileDef {
  tiltLo: number;
  tiltHi: number;
  even: number;
  odd: number;
  maxH: number;
  curve: number;
}

const PROFILES: readonly ProfileDef[] = [
  { tiltLo: 2.0, tiltHi: 1.0, even: 1.0, odd: 1.0, maxH: 48, curve: 3.0 }, // classic
  { tiltLo: 2.4, tiltHi: 1.4, even: 1.3, odd: 0.75, maxH: 12, curve: 2.0 }, // warm
  { tiltLo: 1.6, tiltHi: 1.0, even: 0.0, odd: 1.0, maxH: 48, curve: 3.0 }, // hollow
  { tiltLo: 1.3, tiltHi: 0.6, even: 1.0, odd: 1.0, maxH: 64, curve: 2.5 }, // bright
  { tiltLo: 1.0, tiltHi: 1.0, even: 1.0, odd: 1.0, maxH: 48, curve: 2.0 }, // saw
];

/** Harmonic amplitudes for a profile (0..4) and shape (0..1), index 0 = the fundamental, normalised to the RMS of a
 * unit sine (sum of squares = 1). `bandLimit` is the most harmonics the pitch allows (default: no limit); pass
 * `harmonicBandLimit(f0 / sampleRate)` to see what the engine plays at a pitch. Shape 0 is exactly [1, 0, 0, ...]. */
export function harmonicSpectrum(profile: number, shape: number, bandLimit = Infinity): number[] {
  const p = PROFILES[Math.min(Math.max(Math.round(Number.isFinite(profile) ? profile : 0), 0), PROFILES.length - 1)];
  const s = Math.min(Math.max(Number.isFinite(shape) ? shape : 0, 0), 1);
  const tilt = p.tiltLo + (p.tiltHi - p.tiltLo) * s;
  const heff = Math.max(1, Math.min(1 + (p.maxH - 1) * Math.pow(s, p.curve), bandLimit));
  const top = Math.min(Math.ceil(heff), HARMONIC_MAX);
  const amp = new Array<number>(HARMONIC_MAX).fill(0);
  amp[0] = 1;
  let energy = 1;
  for (let n = 2; n <= top; n++) {
    const open = Math.min(Math.max(heff - (n - 1), 0), 1);
    const a = Math.pow(n, -tilt) * (n % 2 === 1 ? p.odd : p.even) * open;
    amp[n - 1] = a;
    energy += a * a;
  }
  const g = 1 / Math.sqrt(energy);
  return amp.map((a) => a * g);
}

/** The most harmonics a pitch allows, for a per-sample phase increment (f0 / sampleRate). */
export function harmonicBandLimit(phaseIncrement: number, fraction = HARMONIC_BAND_LIMIT): number {
  return Math.max(1, fraction / Math.max(phaseIncrement, 1e-9) - 1);
}

/** One cycle of the waveform (`points` samples, phase 0..1) summed from `harmonicSpectrum`: what the oscillator
 * produces before Drive and the filter. */
export function harmonicWaveform(profile: number, shape: number, points = 256, bandLimit = Infinity): number[] {
  const amp = harmonicSpectrum(profile, shape, bandLimit);
  const out = new Array<number>(points).fill(0);
  for (let n = 1; n <= amp.length; n++) {
    const a = amp[n - 1];
    if (a === 0) continue;
    for (let i = 0; i < points; i++) out[i] += a * Math.sin((2 * Math.PI * n * i) / points);
  }
  return out;
}
