// Compressor display maths and drum-bus presets. Pure data and arithmetic, no DOM and no audio: it
// mirrors native/src/dsp/compressor.h's static curve so a client can draw what the DSP does, and holds
// the preset table so every client starts from the same musical places. See docs/compressor-node.md.

export interface CompressorSettings {
  /** dBFS, -60..0. */
  threshold: number;
  /** n:1, 1..20. */
  ratio: number;
  /** Seconds. */
  attack: number;
  /** Seconds. */
  release: number;
  /** dB, 0..24. */
  knee: number;
  /** dB, -24..24. */
  makeup: number;
}

/** Output level (dB) for an input level (dB): the soft-knee curve of compressor.h, makeup not included. */
export function compressorCurveDb(inDb: number, s: Pick<CompressorSettings, "threshold" | "ratio" | "knee">): number {
  const over = inDb - s.threshold;
  const knee = Math.max(s.knee, 1e-6);
  if (2 * over < -knee) return inDb;
  if (2 * Math.abs(over) <= knee) {
    const t = over + knee / 2;
    return inDb + ((1 / s.ratio - 1) * t * t) / (2 * knee);
  }
  return s.threshold + over / s.ratio;
}

/** Gain reduction (dB, <= 0) the static curve gives a steady level. */
export function staticGainReductionDb(inDb: number, s: Pick<CompressorSettings, "threshold" | "ratio" | "knee">): number {
  return compressorCurveDb(inDb, s) - inDb;
}

export interface CompressorPreset {
  name: string;
  purpose: string;
  ratio: number;
  attack: number;
  release: number;
  knee: number;
  /** Deepest gain reduction on the loudest hits this preset is meant to give, dB (positive). */
  targetGrDb: number;
  /** Where the threshold sits below the loudest hits (dB, positive) to give `targetGrDb` there. It is not the static-curve
   * answer: attack lets the start of every hit through, so the threshold has to be lower than the curve alone suggests. */
  thresholdBelowPeakDb: number;
  /** Explicit makeup, dB: about 0.42 x the target, which measured as equal loudness on a kick/snare/hat/perc pattern. */
  makeup: number;
}

// Calibrated against a kick/snare/hat/perc pattern peaking at -3 dBFS with the real DSP (native/test/compressor_test.cpp,
// testDrumBusPresets, which fails if these drift): the target is the deepest per-block reduction, the makeup restores equal RMS.
export const COMPRESSOR_PRESETS: readonly CompressorPreset[] = [
  { name: "Drum Glue", purpose: "Subtle cohesion across the kit", ratio: 2, attack: 0.025, release: 0.1, knee: 8, targetGrDb: 3, thresholdBelowPeakDb: 11.8, makeup: 1.3 },
  { name: "Punch", purpose: "Keeps the transient, tames the body", ratio: 3, attack: 0.028, release: 0.09, knee: 6, targetGrDb: 4, thresholdBelowPeakDb: 12.2, makeup: 1.7 },
  { name: "Tight", purpose: "Controlled, compact bus", ratio: 4, attack: 0.009, release: 0.07, knee: 6, targetGrDb: 5.5, thresholdBelowPeakDb: 10.8, makeup: 2.3 },
  { name: "Smash", purpose: "Deliberately aggressive", ratio: 10, attack: 0.004, release: 0.05, knee: 2, targetGrDb: 12, thresholdBelowPeakDb: 16, makeup: 5.5 },
];

/** Peak level (dBFS) assumed for a drum bus when nothing is playing to measure: normal webdsp drum levels. */
export const DEFAULT_DRUM_PEAK_DB = -3;

/** A preset as concrete settings for a signal whose loudest hits peak at `peakDb`. */
export function presetSettings(p: CompressorPreset, peakDb: number = DEFAULT_DRUM_PEAK_DB): CompressorSettings {
  return {
    threshold: Math.max(-60, Math.min(0, Math.round((peakDb - p.thresholdBelowPeakDb) * 2) / 2)),
    ratio: p.ratio,
    attack: p.attack,
    release: p.release,
    knee: p.knee,
    makeup: p.makeup,
  };
}

/**
 * The tracked loudness change of the compressor: an energy-weighted mean of (gain reduction + makeup) over the last
 * few seconds. A level-matched bypass applies it to the dry signal, so A/B compares character and not loudness.
 */
export class LoudnessChange {
  private weighted = 0;
  private weight = 0;
  constructor(private readonly decay = 0.97) {}

  /** One meter interval: the compressor's input peak (linear), its reduction (dB) and the makeup in force (dB). */
  push(inPeak: number, grDb: number, makeupDb: number): void {
    const w = inPeak * inPeak;
    this.weighted = this.weighted * this.decay + w * (grDb + makeupDb);
    this.weight = this.weight * this.decay + w;
  }

  /** dB the compressed signal sits above (+) or below (-) the dry one; 0 until something has played. */
  db(): number {
    return this.weight > 1e-9 ? this.weighted / this.weight : 0;
  }

  reset(): void {
    this.weighted = 0;
    this.weight = 0;
  }
}

/** Fixed-length history of gain-reduction values for a trace; oldest first via `values()`. */
export class GainReductionHistory {
  private readonly buf: Float32Array;
  private head = 0;
  constructor(length = 160) {
    this.buf = new Float32Array(length);
  }
  push(grDb: number): void {
    this.buf[this.head] = grDb;
    this.head = (this.head + 1) % this.buf.length;
  }
  /** Oldest to newest. */
  values(): Float32Array {
    const out = new Float32Array(this.buf.length);
    for (let i = 0; i < out.length; i++) out[i] = this.buf[(this.head + i) % this.buf.length];
    return out;
  }
}

/** How much compression a gain reduction is, for a label under the meter. */
export function reductionCharacter(grDb: number): string {
  const g = -grDb;
  if (g < 0.5) return "idle";
  if (g < 2.5) return "light";
  if (g < 4.5) return "moderate";
  if (g < 8) return "firm";
  return g < 12 ? "heavy" : "extreme";
}
