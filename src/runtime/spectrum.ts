// Pure, UI-free spectrum analysis for display. The engine only hands over raw mono samples
// (AudioRuntime.setAnalysisTap / onAnalysis); this turns a window of them into a smoothed
// level-per-column array on a log-frequency axis, ready for any renderer (canvas, SVG, DOM), plus
// the two axis mappings a renderer needs. It knows nothing about EQ, drawing, or the DOM, and
// does no allocation after construction. Run it on the UI side only, never in the audio thread.

import { dbToUnit, logFreqToUnit } from "./scale";

/** Fills `out` with x,y pairs (2 per column, in the units of `width` x `height`, y down) of the
 * analyzer's current levels on a log-frequency axis, `minDb` at the bottom to `maxDb` at the top
 * (clamped). Allocation-free: pass a Float32Array of at least 2 * analyzer.columns. A renderer
 * only has to move to / line to each pair. */
export function spectrumPoints(
  a: SpectrumAnalyzer, width: number, height: number, minDb: number, maxDb: number, out: Float32Array,
): void {
  for (let c = 0; c < a.columns; c++) {
    out[2 * c] = logFreqToUnit(a.frequencies[c], a.minHz, a.maxHz) * width;
    out[2 * c + 1] = height * (1 - Math.min(1, Math.max(0, dbToUnit(a.db[c], minDb, maxDb))));
  }
}

export interface SpectrumOptions {
  /** FFT length, a power of two; the window pushed in must be at least this long. Default 2048. */
  fftSize?: number;
  /** Number of log-spaced output columns. Default 160. */
  columns?: number;
  minHz?: number;
  maxHz?: number;
  /** Level reported for silence. Default -100 dBFS. */
  floorDb?: number;
  /** How fast a column falls once the signal drops, dB per second. Rises are instant. Default 40. */
  fallDbPerSec?: number;
}

export class SpectrumAnalyzer {
  readonly columns: number;
  readonly minHz: number;
  readonly maxHz: number;
  readonly floorDb: number;
  /** Centre frequency of each column, Hz (log-spaced). */
  readonly frequencies: Float32Array;
  /** Smoothed level per column in dBFS (a full-scale sine reads about 0). Updated in place. */
  readonly db: Float32Array;

  private readonly n: number;
  private readonly fallDbPerSec: number;
  private readonly window: Float64Array;
  private readonly re: Float64Array;
  private readonly im: Float64Array;
  private readonly cos: Float64Array;
  private readonly sin: Float64Array;
  private readonly rev: Uint32Array;
  private readonly scale: number;
  private binHz = 0;
  // Per column: the range of FFT bins it covers (lo..hi inclusive) -- or, when narrower than one
  // bin, a fractional position to interpolate at (hi < lo).
  private readonly lo: Int32Array;
  private readonly hi: Int32Array;
  private readonly frac: Float32Array;
  private mapRate = 0;

  constructor(options: SpectrumOptions = {}) {
    const n = (this.n = options.fftSize ?? 2048);
    if (n < 16 || (n & (n - 1)) !== 0) throw new Error("fftSize must be a power of two >= 16");
    this.columns = options.columns ?? 160;
    this.minHz = options.minHz ?? 20;
    this.maxHz = options.maxHz ?? 20000;
    this.floorDb = options.floorDb ?? -100;
    this.fallDbPerSec = options.fallDbPerSec ?? 40;
    this.window = new Float64Array(n);
    let sum = 0;
    for (let i = 0; i < n; i++) sum += this.window[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / n); // Hann
    this.scale = 2 / sum; // |X| * scale = sine amplitude
    this.re = new Float64Array(n);
    this.im = new Float64Array(n);
    this.cos = new Float64Array(n / 2);
    this.sin = new Float64Array(n / 2);
    for (let i = 0; i < n / 2; i++) {
      this.cos[i] = Math.cos((2 * Math.PI * i) / n);
      this.sin[i] = -Math.sin((2 * Math.PI * i) / n);
    }
    this.rev = new Uint32Array(n);
    const bits = Math.log2(n);
    for (let i = 0; i < n; i++) {
      let r = 0;
      for (let b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      this.rev[i] = r;
    }
    this.frequencies = new Float32Array(this.columns);
    this.db = new Float32Array(this.columns).fill(this.floorDb);
    this.lo = new Int32Array(this.columns);
    this.hi = new Int32Array(this.columns);
    this.frac = new Float32Array(this.columns);
    const ratio = Math.log(this.maxHz / this.minHz);
    for (let c = 0; c < this.columns; c++) {
      this.frequencies[c] = this.minHz * Math.exp((ratio * (c + 0.5)) / this.columns);
    }
  }

  private mapColumns(sampleRate: number): void {
    this.mapRate = sampleRate;
    this.binHz = sampleRate / this.n;
    const half = this.n / 2 - 1;
    const ratio = Math.log(this.maxHz / this.minHz);
    for (let c = 0; c < this.columns; c++) {
      const f0 = this.minHz * Math.exp((ratio * c) / this.columns);
      const f1 = this.minHz * Math.exp((ratio * (c + 1)) / this.columns);
      const lo = Math.min(half, Math.ceil(f0 / this.binHz));
      const hi = Math.min(half, Math.floor(f1 / this.binHz));
      this.lo[c] = lo;
      this.hi[c] = hi;
      this.frac[c] = this.frequencies[c] / this.binHz;
    }
  }

  /** Analyses the last `fftSize` samples of `samples` and folds them into `db` (rises instantly,
   * falls at `fallDbPerSec` given `dtSec` since the previous update). */
  push(samples: Float32Array, sampleRate: number, dtSec: number): void {
    if (samples.length < this.n) return;
    if (sampleRate !== this.mapRate) this.mapColumns(sampleRate);
    const n = this.n, off = samples.length - n;
    for (let i = 0; i < n; i++) {
      const r = this.rev[i];
      this.re[r] = samples[off + i] * this.window[i];
      this.im[r] = 0;
    }
    for (let size = 2; size <= n; size <<= 1) {
      const halfSize = size >> 1, step = n / size;
      for (let start = 0; start < n; start += size) {
        for (let j = 0, k = 0; j < halfSize; j++, k += step) {
          const a = start + j, b = a + halfSize;
          const tr = this.re[b] * this.cos[k] - this.im[b] * this.sin[k];
          const ti = this.re[b] * this.sin[k] + this.im[b] * this.cos[k];
          this.re[b] = this.re[a] - tr; this.im[b] = this.im[a] - ti;
          this.re[a] += tr; this.im[a] += ti;
        }
      }
    }
    const fall = this.fallDbPerSec * Math.max(0, dtSec);
    for (let c = 0; c < this.columns; c++) {
      let m2: number;
      if (this.hi[c] >= this.lo[c]) {
        m2 = 0; // loudest bin in the column's range
        for (let b = this.lo[c]; b <= this.hi[c]; b++) m2 = Math.max(m2, this.re[b] * this.re[b] + this.im[b] * this.im[b]);
      } else { // narrower than a bin (low end): interpolate between the neighbours
        const p = this.frac[c], b0 = Math.floor(p), t = p - b0;
        const a = this.re[b0] * this.re[b0] + this.im[b0] * this.im[b0];
        const b = this.re[b0 + 1] * this.re[b0 + 1] + this.im[b0 + 1] * this.im[b0 + 1];
        m2 = a + (b - a) * t;
      }
      const level = m2 > 0 ? 10 * Math.log10(m2) + 20 * Math.log10(this.scale) : this.floorDb;
      const clamped = Math.max(this.floorDb, level);
      this.db[c] = Math.max(clamped, this.db[c] - fall);
    }
  }

  /** Lets the display fall away when no new audio arrives (silence, stopped transport). */
  decay(dtSec: number): void {
    const fall = this.fallDbPerSec * Math.max(0, dtSec);
    for (let c = 0; c < this.columns; c++) this.db[c] = Math.max(this.floorDb, this.db[c] - fall);
  }

  reset(): void {
    this.db.fill(this.floorDb);
  }
}

/** The slice of AudioRuntime a feed needs (structural, so this file stays import-free). */
export interface AnalysisFrame {
  /** Which tap the window came from. */
  bus: number;
  effect: number;
  samples: Float32Array;
  sampleRate: number;
}

export interface SpectrumSource {
  setAnalysisTap(bus: number, effect: number | null): void;
  releaseAnalysisTap(bus: number, effect: number): void;
  onAnalysis(fn: (frame: AnalysisFrame) => void): () => void;
}

/**
 * Connects an engine tap to a SpectrumAnalyzer for one display. The message handler only stores
 * the latest window (no work); the FFT runs inside `frame()`, at most once per animation frame,
 * so a hidden tab or an unmounted display costs nothing and a burst of messages is coalesced.
 *
 *   const feed = new SpectrumFeed(runtime, bus, effectId, analyzer);
 *   // in requestAnimationFrame:  if (feed.frame(performance.now())) draw(analyzer);
 *   // on unmount:                feed.dispose();
 *
 * `frame()` returns true when `analyzer.db` changed and the display should be redrawn; it returns
 * false once everything has settled at the floor, so an idle display does no drawing at all.
 */
export class SpectrumFeed {
  private pending: Float32Array | null = null;
  private pendingRate = 0;
  private lastMs: number | null = null;
  private settled = false;
  private readonly unsubscribe: () => void;

  constructor(
    private readonly source: SpectrumSource,
    private readonly bus: number,
    private readonly effect: number,
    private readonly analyzer: SpectrumAnalyzer,
  ) {
    this.unsubscribe = source.onAnalysis((f) => {
      if (f.bus !== this.bus || f.effect !== this.effect) return; // another display's tap
      this.pending = f.samples;
      this.pendingRate = f.sampleRate;
    });
    source.setAnalysisTap(bus, effect);
  }

  frame(nowMs: number): boolean {
    const dt = this.lastMs === null ? 0 : Math.min(0.25, Math.max(0, (nowMs - this.lastMs) / 1000));
    this.lastMs = nowMs;
    if (this.pending) {
      this.analyzer.push(this.pending, this.pendingRate, dt);
      this.pending = null;
      this.settled = false;
      return true;
    }
    if (this.settled) return false;
    this.analyzer.decay(dt);
    let floor = true;
    for (let i = 0; i < this.analyzer.columns; i++) if (this.analyzer.db[i] > this.analyzer.floorDb) { floor = false; break; }
    this.settled = floor;
    return true;
  }

  dispose(): void {
    this.unsubscribe();
    this.source.releaseAnalysisTap(this.bus, this.effect);
    this.pending = null;
  }
}
