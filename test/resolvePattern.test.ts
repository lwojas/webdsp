import { describe, expect, it } from "vitest";
import { resolvePattern } from "../src/app/patterns/resolvePattern";
import type { RolePattern } from "../src/app/patterns/testPatterns";

describe("resolvePattern", () => {
  const pattern: RolePattern = [
    [{ role: "kick", velocity: 1.0 }, { role: "hat", velocity: 0.5 }],
    [{ role: "snare" }],
  ];

  it("substitutes each role with its assigned SampleId", () => {
    const resolved = resolvePattern(pattern, { kick: 10, snare: 20, hat: 30, perc: null });
    expect(resolved[0]).toEqual([
      { sampleId: 10, velocity: 1.0, pitch: undefined },
      { sampleId: 30, velocity: 0.5, pitch: undefined },
    ]);
    expect(resolved[1]).toEqual([{ sampleId: 20, velocity: undefined, pitch: undefined }]);
  });

  it("drops notes whose role has no assignment rather than erroring", () => {
    const resolved = resolvePattern(pattern, { kick: 10, snare: null, hat: null, perc: null });
    expect(resolved[0]).toHaveLength(1);
    expect(resolved[0][0].sampleId).toBe(10);
    expect(resolved[1]).toHaveLength(0);
  });
});
