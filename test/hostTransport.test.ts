import { describe, expect, it } from "vitest";
import { hasEmbeddedHost, hostBeatAt, hostTimeAt, isHostDiscontinuity, type HostTransport } from "../src/runtime/hostTransport";

const base: HostTransport = { playing: true, bpm: 120, beat: 8, time: 10, loop: null, timeSignature: null, epoch: 1 };

describe("host transport mapping", () => {
  it("extrapolates beat from time at the host tempo", () => {
    expect(hostBeatAt(base, 10)).toBe(8);
    expect(hostBeatAt(base, 11)).toBeCloseTo(10, 9); // 120 bpm = 2 beats per second
    expect(hostBeatAt({ ...base, bpm: 90 }, 12)).toBeCloseTo(11, 9);
  });

  it("is the inverse of hostBeatAt", () => {
    for (const t of [9.5, 10, 13.37]) expect(hostTimeAt(base, hostBeatAt(base, t))).toBeCloseTo(t, 9);
  });

  it("calls a start, stop or new epoch a discontinuity, and a tempo change not", () => {
    expect(isHostDiscontinuity(null, base)).toBe(true);
    expect(isHostDiscontinuity(base, { ...base, playing: false })).toBe(true);
    expect(isHostDiscontinuity(base, { ...base, epoch: 2 })).toBe(true);
    expect(isHostDiscontinuity(base, { ...base, bpm: 121, beat: 9, time: 10.5 })).toBe(false);
  });
});

describe("embedded host marker", () => {
  it("is false in a plain browser/node and true once a host sets the global", () => {
    const g = globalThis as { __webdspHost?: unknown };
    expect(hasEmbeddedHost()).toBe(false);
    g.__webdspHost = { kind: "native" };
    expect(hasEmbeddedHost()).toBe(true);
    delete g.__webdspHost;
  });
});
