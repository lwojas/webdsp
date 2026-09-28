import { describe, expect, it } from "vitest";
import { compilePattern } from "../src/sequencing/pattern";

describe("compilePattern", () => {
  it("emits nothing for empty steps and one event per note otherwise", () => {
    const events = compilePattern(
      [[{ sampleId: 1 }], [], [{ sampleId: 2 }, { sampleId: 3 }]],
      { startTime: 10, stepDurationSeconds: 0.25 },
    );
    expect(events).toHaveLength(3);
    expect(events.map((e) => e.sampleId)).toEqual([1, 2, 3]);
  });

  it("places each step at startTime + stepIndex * stepDuration", () => {
    const events = compilePattern([[{ sampleId: 1 }], [{ sampleId: 1 }], [{ sampleId: 1 }]], {
      startTime: 2,
      stepDurationSeconds: 0.5,
    });
    expect(events.map((e) => e.time)).toEqual([2, 2.5, 3]);
  });

  it("repeats the whole pattern for `loops` and offsets each repeat by the pattern duration", () => {
    const events = compilePattern([[{ sampleId: 1 }], []], {
      startTime: 0,
      stepDurationSeconds: 0.1,
      loops: 3,
    });
    expect(events.map((e) => e.time)).toEqual([0, 0.2, 0.4]);
  });

  it("maps velocity to gain and passes pitch through, defaulting gain to 1.0", () => {
    const events = compilePattern([[{ sampleId: 1, velocity: 0.5, pitch: 7 }, { sampleId: 2 }]], {
      startTime: 0,
      stepDurationSeconds: 1,
    });
    expect(events[0]).toMatchObject({ sampleId: 1, gain: 0.5, pitch: 7 });
    expect(events[1]).toMatchObject({ sampleId: 2, gain: 1.0 });
    expect(events[1].pitch).toBeUndefined();
  });
});
