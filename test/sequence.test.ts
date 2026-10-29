import { describe, expect, it } from "vitest";
import { normalizeSequence, type Sequence } from "../src/runtime/sequence";

const known = { sample: (id: number) => id === 5, instrument: (id: number) => id === 7, effect: (bus: number, fx: number) => bus === 0 && fx === 3, bus: (bus: number) => bus === 2 };

describe("normalizeSequence", () => {
  it("sorts by beat, stably, and keeps what applies", () => {
    const seq: Sequence = {
      lengthBeats: 4,
      events: [
        { beat: 2, kind: "sample", sample: 5, gain: 0.5, mono: true, track: 1 },
        { beat: 0, kind: "instrument", instrument: 7, velocity: 2, durationBeats: 0.5, tie: true },
        { beat: 0, kind: "sample", sample: 5 },
      ],
    };
    const out = normalizeSequence(seq, known)!;
    expect(out.events.map((e) => e.kind)).toEqual(["instrument", "sample", "sample"]);
    expect(out.events[0]).toMatchObject({ velocity: 1, durationBeats: 0.5, tie: true });
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

  it("keeps bus (mixer) param events for known buses only, and drops them without a bus check", () => {
    const seq: Sequence = {
      lengthBeats: 4,
      events: [
        { beat: 1, kind: "param", target: { bus: 2 }, param: 71, value: 0.5 },
        { beat: 1, kind: "param", target: { bus: 9 }, param: 71, value: 0.5 },
        { beat: 2, kind: "param", target: { bus: 2 }, param: 71, value: Number.NaN },
      ],
    };
    expect(normalizeSequence(seq, known)!.events).toEqual([{ beat: 1, kind: "param", target: { bus: 2 }, param: 71, value: 0.5 }]);
    const { bus: _unused, ...noBus } = known;
    expect(normalizeSequence(seq, noBus)).toBeNull();
  });

  it("returns null when nothing is playable", () => {
    expect(normalizeSequence(null, known)).toBeNull();
    expect(normalizeSequence({ lengthBeats: 0, events: [{ beat: 0, kind: "sample", sample: 5 }] }, known)).toBeNull();
    expect(normalizeSequence({ lengthBeats: 4, events: [] }, known)).toBeNull();
    expect(normalizeSequence({ lengthBeats: 4, events: [{ beat: 0, kind: "sample", sample: 99 }] }, known)).toBeNull();
  });

  describe("loops (polymeter)", () => {
    it("keeps each loop with its own length, cleaned like the main cycle", () => {
      const out = normalizeSequence(
        {
          lengthBeats: 4,
          events: [{ beat: 0, kind: "instrument", instrument: 7 }],
          loops: [
            { lengthBeats: 3.25, events: [{ beat: 3, kind: "instrument", instrument: 7 }, { beat: 3.25, kind: "instrument", instrument: 7 }, { beat: 0, kind: "sample", sample: 99 }] },
            { lengthBeats: 0, events: [{ beat: 0, kind: "instrument", instrument: 7 }] }, // no length: dropped
            { lengthBeats: 1.75, events: [] }, // nothing to play: dropped
          ],
        },
        known,
      )!;
      expect(out.loops).toEqual([{ lengthBeats: 3.25, events: [{ beat: 3, kind: "instrument", instrument: 7 }] }]);
    });

    it("plays from loops alone, with an empty main cycle", () => {
      const out = normalizeSequence({ lengthBeats: 4, events: [], loops: [{ lengthBeats: 1.5, events: [{ beat: 0, kind: "instrument", instrument: 7 }] }] }, known)!;
      expect(out.events).toEqual([]);
      expect(out.loops).toHaveLength(1);
      expect(normalizeSequence({ lengthBeats: 4, events: [], loops: [] }, known)).toBeNull();
    });

    it("a sequence without loops is unchanged (no loops key)", () => {
      expect(normalizeSequence({ lengthBeats: 1, events: [{ beat: 0, kind: "instrument", instrument: 7 }] }, known)).not.toHaveProperty("loops");
    });
  });
});
