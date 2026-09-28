import type { SampleId, ScheduledEvent } from "../runtime/types";

// A deliberately simple, illustrative sequence representation (see ARCHITECTURE.md,
// "Test sequence representation" / project brief section 9). This is NOT part of the audio
// runtime's API and is not meant to be the permanent shape of a future tracker's data model
// — it exists only to prove that an external, generic-event-producing sequencer can drive
// the runtime without the runtime knowing anything about "patterns" or "steps".
export interface PatternNote {
  sampleId: SampleId;
  velocity?: number;
  pitch?: number;
}

export type Pattern = PatternNote[][];

export interface CompileOptions {
  /** Absolute runtime time (seconds, same domain as AudioRuntime.getCurrentTime()) of
   * step 0. */
  startTime: number;
  stepDurationSeconds: number;
  /** How many times to repeat the whole pattern. Default 1 (no repeat). */
  loops?: number;
}

function compileStep(step: PatternNote[], time: number): ScheduledEvent[] {
  return step.map((note) => ({
    time,
    sampleId: note.sampleId,
    gain: note.velocity ?? 1.0,
    pitch: note.pitch,
  }));
}

/** Eagerly compiles an entire (finite) pattern run into generic ScheduledEvents the
 * runtime understands. Fine for a bounded number of loops; for an indefinitely-looping
 * pattern, use LookaheadPlayer instead so events aren't scheduled unboundedly far ahead. */
export function compilePattern(pattern: Pattern, opts: CompileOptions): ScheduledEvent[] {
  const loops = opts.loops ?? 1;
  const patternDuration = pattern.length * opts.stepDurationSeconds;
  const events: ScheduledEvent[] = [];
  for (let loop = 0; loop < loops; loop++) {
    pattern.forEach((step, stepIndex) => {
      const time = opts.startTime + loop * patternDuration + stepIndex * opts.stepDurationSeconds;
      events.push(...compileStep(step, time));
    });
  }
  return events;
}

export { compileStep };
