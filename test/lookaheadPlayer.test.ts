import { describe, expect, it, vi } from "vitest";
import type { AudioRuntime } from "../src/runtime";
import type { ScheduledEvent } from "../src/runtime/types";
import { LookaheadPlayer } from "../src/sequencing/lookaheadPlayer";

// AudioRuntime needs a real browser (AudioContext/AudioWorklet), so it's not unit-tested
// directly here — see ARCHITECTURE.md, "Automated tests for the non-realtime parts", for
// why that seam is covered by the native ABI smoke test plus manual browser testing
// instead. LookaheadPlayer's own scheduling logic, though, is pure enough to fake the
// runtime out entirely and test deterministically.
function fakeRuntime() {
  let time = 0;
  const scheduled: ScheduledEvent[] = [];
  const runtime = {
    getCurrentTime: () => time,
    schedule: vi.fn((events: ScheduledEvent[]) => {
      scheduled.push(...events);
      return events.map((_, i) => i + 1);
    }),
    cancelScheduled: vi.fn(),
  };
  return { runtime: runtime as unknown as AudioRuntime, scheduled, advance: (dt: number) => (time += dt) };
}

describe("LookaheadPlayer", () => {
  it("only schedules steps that fall within the lookahead window, then tops up on each tick", () => {
    vi.useFakeTimers();
    const { runtime, scheduled, advance } = fakeRuntime();
    const pattern = [[{ sampleId: 1 }], [{ sampleId: 2 }], [{ sampleId: 3 }]];
    const player = new LookaheadPlayer(runtime, pattern, 0.1, true, 0.15, 25);

    player.start(0);
    expect(scheduled.map((e) => e.sampleId)).toEqual([1, 2]);

    advance(0.1);
    vi.advanceTimersByTime(25);
    expect(scheduled.map((e) => e.sampleId)).toEqual([1, 2, 3]);

    player.stop();
    vi.useRealTimers();
  });

  it("derives the playhead step from the runtime's own clock, not the timer", () => {
    vi.useFakeTimers();
    const { runtime, advance } = fakeRuntime();
    const pattern = [[{ sampleId: 1 }], [{ sampleId: 2 }], [{ sampleId: 3 }]];
    const player = new LookaheadPlayer(runtime, pattern, 0.1, true);

    player.start(0);
    expect(player.getPlayheadStep()).toBe(0);

    advance(0.25); // no timer tick at all — playhead still reflects engine time
    expect(player.getPlayheadStep()).toBe(2);

    player.stop();
    vi.useRealTimers();
  });

  it("cancels scheduled events on stop and reports not-playing", () => {
    vi.useFakeTimers();
    const { runtime } = fakeRuntime();
    const player = new LookaheadPlayer(runtime, [[{ sampleId: 1 }]], 0.1, true);

    player.start(0);
    expect(player.isPlaying()).toBe(true);

    player.stop();
    expect(player.isPlaying()).toBe(false);
    expect(runtime.cancelScheduled).toHaveBeenCalledOnce();
    expect(player.getPlayheadStep()).toBe(-1);

    vi.useRealTimers();
  });

  it("stops itself after one pass when loop is false", () => {
    vi.useFakeTimers();
    const { runtime, scheduled } = fakeRuntime();
    const player = new LookaheadPlayer(runtime, [[{ sampleId: 1 }], [{ sampleId: 2 }]], 0.1, false, 1.0);

    player.start(0);
    expect(scheduled.map((e) => e.sampleId)).toEqual([1, 2]);
    expect(player.isPlaying()).toBe(false);

    vi.useRealTimers();
  });
});
