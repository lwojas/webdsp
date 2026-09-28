import type { AudioRuntime } from "../runtime";
import { compileStep, type Pattern } from "./pattern";

// Classic "lookahead scheduler" (see Chris Wilson's "A Tale of Two Clocks"): a JS timer
// decides *when to top up the schedule* by polling every tickIntervalMs, but the actual
// playback timing comes entirely from the absolute engine times handed to
// AudioRuntime.schedule() — the engine, not this timer, executes those sample-accurately.
// This is what makes it safe to use setInterval here without violating "don't use JS timers
// as the audio clock": the timer only ever decides how far ahead to push events into a
// clock it does not control. See ARCHITECTURE.md, "Scheduling".
export class LookaheadPlayer {
  private timerId: ReturnType<typeof setInterval> | null = null;
  private nextStepIndex = 0;
  private loopStartTime = 0;

  constructor(
    private readonly runtime: AudioRuntime,
    private pattern: Pattern,
    private readonly stepDurationSeconds: number,
    private readonly loop: boolean = true,
    private readonly lookaheadSeconds = 0.15,
    private readonly tickIntervalMs = 25,
  ) {}

  start(startAt?: number): void {
    this.loopStartTime = startAt ?? this.runtime.getCurrentTime() + 0.05;
    this.nextStepIndex = 0;
    // The interval is armed *before* the initial tick() runs, not after: a non-looping
    // pattern that fits entirely within the first lookahead window causes tick() to call
    // stop() synchronously, and stop() only clears a timer that's already set. Calling
    // tick() first would let a later `this.timerId = setInterval(...)` resurrect a player
    // that had just legitimately stopped.
    this.timerId = setInterval(() => this.tick(), this.tickIntervalMs);
    this.tick();
  }

  stop(): void {
    if (this.timerId !== null) {
      clearInterval(this.timerId);
      this.timerId = null;
    }
    this.runtime.cancelScheduled();
  }

  isPlaying(): boolean {
    return this.timerId !== null;
  }

  /** Current step index for playhead visualization. Purely derived from the engine's own
   * clock (getCurrentTime()) — never the timer above — so a slow/throttled UI thread never
   * desyncs audio, only the visual playhead's redraw rate. Returns -1 before playback
   * has audibly started (e.g. during the initial scheduling lead-in). */
  getPlayheadStep(): number {
    if (this.timerId === null) return -1;
    const elapsed = this.runtime.getCurrentTime() - this.loopStartTime;
    if (elapsed < 0) return -1;
    const step = Math.floor(elapsed / this.stepDurationSeconds);
    return this.loop ? step % this.pattern.length : Math.min(step, this.pattern.length - 1);
  }

  private tick(): void {
    const horizon = this.runtime.getCurrentTime() + this.lookaheadSeconds;
    while (this.stepTime(this.nextStepIndex) < horizon) {
      if (!this.loop && this.nextStepIndex >= this.pattern.length) {
        this.stop();
        return;
      }
      const patternIndex = this.nextStepIndex % this.pattern.length;
      const time = this.stepTime(this.nextStepIndex);
      const notes = this.pattern[patternIndex];
      if (notes.length > 0) this.runtime.schedule(compileStep(notes, time));
      this.nextStepIndex++;
    }
  }

  private stepTime(index: number): number {
    return this.loopStartTime + index * this.stepDurationSeconds;
  }
}
