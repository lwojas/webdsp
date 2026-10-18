// Cue-point helpers for chop playback — pure data maths, no engine, DOM or storage. An app keeps
// its cue points with the sample in its own project as sorted fractions (0..1) of the sample
// length (interior slice boundaries only, so they survive a different engine sample rate), edits
// them with these helpers, and converts them to frame regions for AudioRuntime.setSampleRegions()
// with cueRegions(). Slices are contiguous: boundaries [0, ...cues, 1] give N cues -> N+1 slices.
// Playback mode, note mapping and UI stay with the app (see docs/sample-chop.md).

/** Smallest slice, as a fraction of the sample; keeps neighbouring cues from collapsing. */
export const MIN_SLICE = 0.002;

/** Most slices the engine can hold per sample (native `kMaxSampleRegions`). Apps may pass a
 * smaller cap, e.g. to keep every slice reachable from their note range. */
export const MAX_SLICES = 256;

/** Sorted, de-duplicated, in-range cues with every slice at least MIN_SLICE wide, capped to
 * `maxSlices` slices. Non-finite values are dropped. */
export function normalizeCues(cues: readonly number[], maxSlices = MAX_SLICES): number[] {
  const sorted = cues
    .filter((c) => Number.isFinite(c))
    .map((c) => Math.min(1, Math.max(0, c)))
    .sort((a, b) => a - b);
  const result: number[] = [];
  let prev = 0;
  for (const c of sorted) {
    if (c - prev < MIN_SLICE || 1 - c < MIN_SLICE) continue;
    if (result.length >= maxSlices - 1) break;
    result.push(c);
    prev = c;
  }
  return result;
}

/** Evenly spaced cues giving `slices` equal slices (1 = no cues). */
export function evenCues(slices: number, maxSlices = MAX_SLICES): number[] {
  const n = Math.min(maxSlices, Math.max(1, Math.round(slices)));
  return Array.from({ length: n - 1 }, (_, i) => (i + 1) / n);
}

/** Number of slices `cues` makes: 0 when there are no cues (the sample is not sliced). */
export function sliceCount(cues: readonly number[] | null | undefined): number {
  return cues && cues.length > 0 ? cues.length + 1 : 0;
}

/** Adds a cue at `at` (fraction). A position too close to an existing cue or an edge is ignored. */
export function addCue(cues: readonly number[], at: number, maxSlices = MAX_SLICES): number[] {
  return normalizeCues([...cues, at], maxSlices);
}

/** Moves cue `index` to `to`, clamped between its neighbours so cues never cross. */
export function moveCue(cues: readonly number[], index: number, to: number): number[] {
  if (index < 0 || index >= cues.length) return [...cues];
  const lo = (index > 0 ? cues[index - 1] : 0) + MIN_SLICE;
  const hi = (index < cues.length - 1 ? cues[index + 1] : 1) - MIN_SLICE;
  const next = [...cues];
  next[index] = Math.min(hi, Math.max(lo, to));
  return next;
}

export function removeCue(cues: readonly number[], index: number): number[] {
  return cues.filter((_, i) => i !== index);
}

/** The frame regions for `cues` over a sample of `length` frames, ready for setSampleRegions(). */
export function cueRegions(cues: readonly number[], length: number): { start: number; end: number }[] {
  const bounds = [0, ...cues, 1].map((f) => Math.round(f * length));
  return bounds.slice(0, -1).map((start, i) => ({ start, end: bounds[i + 1] }));
}

/** Peak |amplitude| (0..1) of `bins` equal spans across all channels — a waveform overview.
 * Decoding the audio to channel arrays is the caller's job. */
export function computePeaks(channels: readonly ArrayLike<number>[], bins = 1024): Float32Array {
  const peaks = new Float32Array(bins);
  const length = channels[0]?.length ?? 0;
  if (length === 0) return peaks;
  for (const data of channels) {
    for (let b = 0; b < bins; b++) {
      const from = Math.floor((b * length) / bins);
      const to = Math.max(from + 1, Math.floor(((b + 1) * length) / bins));
      let max = peaks[b];
      for (let i = from; i < to && i < length; i++) {
        const v = Math.abs(data[i]);
        if (v > max) max = v;
      }
      peaks[b] = max;
    }
  }
  return peaks;
}
