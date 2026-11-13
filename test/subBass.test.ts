import { describe, expect, it } from "vitest";
import { HARMONIC_MAX, harmonicBandLimit, harmonicSpectrum, harmonicWaveform } from "../src/runtime";

// The same numbers native/test/subbass_test.cpp (testHarmonicSpectrum, "mirror" block) checks against the real DSP,
// so the client-side waveform and the engine cannot drift apart without one of the two suites failing.
describe("Sub Bass harmonic spectrum (client-side mirror)", () => {
  it("shape 0 is exactly a sine in every profile", () => {
    for (let p = 0; p < 5; p++) {
      const a = harmonicSpectrum(p, 0);
      expect(a[0]).toBe(1);
      expect(a.slice(1).every((v) => v === 0)).toBe(true);
      expect(harmonicWaveform(p, 0, 64).every((v, i) => Math.abs(v - Math.sin((2 * Math.PI * i) / 64)) < 1e-12)).toBe(true);
    }
  });

  it("matches the native values", () => {
    const expectClose = (got: number[], want: number[]) => want.forEach((w, i) => expect(got[i]).toBeCloseTo(w, 5));
    expectClose(harmonicSpectrum(0, 0.5), [0.915728, 0.323759, 0.176232, 0.114466, 0.081905, 0.062307, 0.043264, 0]);
    expectClose(harmonicSpectrum(1, 0.3), [0.957773, 0.287527, 0, 0, 0, 0, 0, 0]);
    expectClose(harmonicSpectrum(3, 0.8, 12.5), [0.693326, 0.415121, 0.307516, 0.24855, 0.210717, 0.184122, 0.164272, 0.148816]);
  });

  it("is RMS-normalised, a 1/n saw at the top of the saw profile, odd-only for hollow", () => {
    for (const [p, s] of [[0, 0.3], [1, 1], [2, 0.7], [3, 1], [4, 0.5]]) {
      const e = harmonicSpectrum(p, s).reduce((sum, v) => sum + v * v, 0);
      expect(e).toBeCloseTo(1, 5);
    }
    const saw = harmonicSpectrum(4, 1);
    expect(saw[1] / saw[0]).toBeCloseTo(0.5, 5);
    expect(saw[9] / saw[0]).toBeCloseTo(0.1, 5);
    const hollow = harmonicSpectrum(2, 1);
    expect(hollow[1]).toBe(0);
    expect(hollow[2]).toBeGreaterThan(0);
  });

  it("opens harmonics in order and never past the band limit", () => {
    const a = harmonicSpectrum(0, 0.25);
    expect(a[1]).toBeGreaterThan(0);
    expect(a[3]).toBe(0);
    expect(harmonicSpectrum(4, 1, 10).filter((v) => v > 0)).toHaveLength(10);
    // 55 Hz at 48 kHz: plenty of room; 1.7 kHz: only a handful of harmonics fit below 0.42 fs.
    expect(harmonicBandLimit(55 / 48000)).toBeGreaterThan(HARMONIC_MAX);
    expect(harmonicBandLimit(1760 / 48000)).toBeLessThan(11);
  });

  it("tolerates out-of-range input", () => {
    expect(harmonicSpectrum(99, 5)).toHaveLength(HARMONIC_MAX);
    expect(harmonicSpectrum(-3, Number.NaN)[0]).toBe(1);
  });
});
