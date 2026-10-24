import { describe, expect, it } from "vitest";
import { normalizeSequence, type Sequence } from "../src/runtime/sequence";

const known = { sample: (id: number) => id === 5, instrument: (id: number) => id === 7, effect: (bus: number, fx: number) => bus === 0 && fx === 3 };

describe("normalizeSequence", () => {
  it("sorts by beat, stably, and keeps what applies", () => {
    const seq: Sequence = {
      lengthBeats: 4,
      events: [
        { beat: 2, kind: "sample", sample: 5, gain: 0.5, mono: true, track: 1 },
        { beat: 0, kind: "instrument", instrument: 7, velocity: 2, durationBeats: 0.5 },
        { beat: 0, kind: "sample", sample: 5 },
      ],
    };
    const out = normalizeSequence(seq, known)!;
    expect(out.events.map((e) => e.kind)).toEqual(["instrument", "sample", "sample"]);
    expect(out.events[0]).toMatchObject({ velocity: 1, durationBeats: 0.5 });
    expect(out.events[2]).toMatchObject({ gain: 0.5, mono: true, track: 1 });
  });

  it("drops unknown targets, bad beats and non-finite numbers", () => {
    const out = normalizeSequence(
      {
        lengthBeats: 4,
        events: [
          { beat: 0, kind: "sample", sample: 99 },
          { beat: 4, kind: "sample", sample: 5 }, // at the end of the cycle: outside
          { beat: -1, kind: "sample", sample: 5 },
          { beat: Number.NaN, kind: "sample", sample: 5 },
          { beat: 1, kind: "param", target: { effect: { bus: 0, effect: 9 } }, param: 0, value: 1 },
          { beat: 1, kind: "param", target: { effect: { bus: 0, effect: 3 } }, param: 0, value: Number.NaN },
          { beat: 1, kind: "param", target: { effect: { bus: 0, effect: 3 } }, param: 2, value: 0.4, from: 0.1, rampBeats: 1 },
          { beat: 3, kind: "sample", sample: 5, durationBeats: -2 },
        ],
      },
      known,
    )!;
    expect(out.events).toEqual([
      { beat: 1, kind: "param", target: { effect: { bus: 0, effect: 3 } }, param: 2, value: 0.4, from: 0.1, rampBeats: 1 },
      { beat: 3, kind: "sample", sample: 5 },
    ]);
  });

  it("returns null when nothing is playable", () => {
    expect(normalizeSequence(null, known)).toBeNull();
    expect(normalizeSequence({ lengthBeats: 0, events: [{ beat: 0, kind: "sample", sample: 5 }] }, known)).toBeNull();
    expect(normalizeSequence({ lengthBeats: 4, events: [] }, known)).toBeNull();
    expect(normalizeSequence({ lengthBeats: 4, events: [{ beat: 0, kind: "sample", sample: 99 }] }, known)).toBeNull();
  });
});
