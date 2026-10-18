import { describe, expect, it } from "vitest";
import { regionForNote } from "../src/runtime/types";

describe("regionForNote", () => {
  it("maps consecutive notes onto consecutive regions from the root note", () => {
    expect(regionForNote(48, 48, 8)).toBe(0);
    expect(regionForNote(55, 48, 8)).toBe(7);
  });
  it("returns undefined outside the region range", () => {
    expect(regionForNote(47, 48, 8)).toBeUndefined();
    expect(regionForNote(56, 48, 8)).toBeUndefined();
    expect(regionForNote(48, 48, 0)).toBeUndefined();
  });
});
