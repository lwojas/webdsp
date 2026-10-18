import { describe, expect, it } from "vitest";
import { addCue, computePeaks, cueRegions, evenCues, MAX_SLICES, MIN_SLICE, moveCue, normalizeCues, removeCue, sliceCount } from "../src/runtime/chop";

describe("cue helpers", () => {
  it("makes N equal slices from N-1 cues and converts to frame regions", () => {
    expect(evenCues(4)).toEqual([0.25, 0.5, 0.75]);
    expect(evenCues(1)).toEqual([]);
    expect(cueRegions([0.5], 100)).toEqual([{ start: 0, end: 50 }, { start: 50, end: 100 }]);
    expect(cueRegions([], 100)).toEqual([{ start: 0, end: 100 }]);
  });

  it("normalises: sorted, in range, de-duplicated, minimum slice width", () => {
    expect(normalizeCues([0.6, 0.2, 0.2, 1.5, -1, NaN])).toEqual([0.2, 0.6]);
    expect(normalizeCues([0.5, 0.5 + MIN_SLICE / 2])).toEqual([0.5]);
  });

  it("honours a smaller slice cap and defaults to the engine cap", () => {
    expect(evenCues(100, 8)).toHaveLength(7);
    expect(addCue([0.25, 0.5, 0.75], 0.9, 4)).toEqual([0.25, 0.5, 0.75]);
    expect(evenCues(10_000)).toHaveLength(MAX_SLICES - 1);
  });

  it("adds, moves (never crossing neighbours) and removes cues", () => {
    expect(addCue([0.5], 0.25)).toEqual([0.25, 0.5]);
    expect(moveCue([0.25, 0.5], 0, 0.9)[0]).toBeCloseTo(0.5 - MIN_SLICE, 6);
    expect(moveCue([0.25, 0.5], 1, 0.1)[1]).toBeCloseTo(0.25 + MIN_SLICE, 6);
    expect(moveCue([0.25], 5, 0.1)).toEqual([0.25]);
    expect(removeCue([0.25, 0.5], 0)).toEqual([0.5]);
  });

  it("counts slices (0 when unsliced)", () => {
    expect(sliceCount([])).toBe(0);
    expect(sliceCount(undefined)).toBe(0);
    expect(sliceCount([0.5])).toBe(2);
  });

  it("reduces channels to per-bin peaks", () => {
    const peaks = computePeaks([new Float32Array([0, 0.5, -0.9, 0.1])], 2);
    expect(peaks[0]).toBeCloseTo(0.5);
    expect(peaks[1]).toBeCloseTo(0.9);
    expect(computePeaks([], 4)).toEqual(new Float32Array(4));
  });
});
