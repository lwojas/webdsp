import { describe, expect, it } from "vitest";
import { normalizeMidiRoute, sameMidiRoute, type MidiRoute } from "../src/runtime/midiRoute";

const known = { sample: (id: number) => id === 5, instrument: (id: number) => id === 7 };

describe("normalizeMidiRoute", () => {
  it("passes a good sample route through, keeping only what applies", () => {
    const route: MidiRoute = { kind: "sample", id: 5, unityNote: 60, bus: 3, chop: true, loop: true, mono: true, holdUntilKeyUp: true, gain: 0.5, chokeGroup: 2, channel: 9 };
    expect(normalizeMidiRoute(route, known)).toEqual(route);
  });

  it("drops the sample-only fields from an instrument route", () => {
    const route = { kind: "instrument", id: 7, unityNote: 36, bus: 3, chop: true, loop: true, mono: true, holdUntilKeyUp: true } as MidiRoute;
    expect(normalizeMidiRoute(route, known)).toEqual({ kind: "instrument", id: 7, unityNote: 36, holdUntilKeyUp: true });
  });

  it("refuses null, an unknown target, a wrong kind and non-finite numbers", () => {
    expect(normalizeMidiRoute(null, known)).toBeNull();
    expect(normalizeMidiRoute(undefined, known)).toBeNull();
    expect(normalizeMidiRoute({ kind: "sample", id: 99, unityNote: 60 }, known)).toBeNull(); // no such sample
    expect(normalizeMidiRoute({ kind: "instrument", id: 5, unityNote: 60 }, known)).toBeNull(); // 5 is a sample, not an instrument
    expect(normalizeMidiRoute({ kind: "wave" as MidiRoute["kind"], id: 5, unityNote: 60 }, known)).toBeNull();
    expect(normalizeMidiRoute({ kind: "sample", id: 5, unityNote: Number.NaN }, known)).toBeNull();
    expect(normalizeMidiRoute({ kind: "sample", id: Number.POSITIVE_INFINITY, unityNote: 60 }, known)).toBeNull();
  });

  it("clamps and rounds, and ignores values out of range", () => {
    const route = normalizeMidiRoute({ kind: "sample", id: 5, unityNote: 200.4, gain: 3, chokeGroup: 0, channel: 16, bus: -1 }, known)!;
    expect(route.unityNote).toBe(127);
    expect(route.gain).toBe(1);
    expect(route.chokeGroup).toBeUndefined();
    expect(route.channel).toBeUndefined();
    expect(route.bus).toBeUndefined();
    expect(normalizeMidiRoute({ kind: "sample", id: 5, unityNote: -4, gain: -1 }, known)).toMatchObject({ unityNote: 0, gain: 0 });
  });
});

describe("sameMidiRoute", () => {
  it("is true for equal routes and for two nulls, false otherwise", () => {
    const a: MidiRoute = { kind: "instrument", id: 7, unityNote: 60 };
    expect(sameMidiRoute(a, { ...a })).toBe(true);
    expect(sameMidiRoute(null, null)).toBe(true);
    expect(sameMidiRoute(a, { ...a, unityNote: 61 })).toBe(false);
    expect(sameMidiRoute(a, null)).toBe(false);
  });
});
