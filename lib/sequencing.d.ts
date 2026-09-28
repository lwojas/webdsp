import { SampleId, ScheduledEvent, AudioRuntime } from './index.js';

interface PatternNote {
    sampleId: SampleId;
    velocity?: number;
    pitch?: number;
}
type Pattern = PatternNote[][];
interface CompileOptions {
    /** Absolute runtime time (seconds, same domain as AudioRuntime.getCurrentTime()) of
     * step 0. */
    startTime: number;
    stepDurationSeconds: number;
    /** How many times to repeat the whole pattern. Default 1 (no repeat). */
    loops?: number;
}
declare function compileStep(step: PatternNote[], time: number): ScheduledEvent[];
/** Eagerly compiles an entire (finite) pattern run into generic ScheduledEvents the
 * runtime understands. Fine for a bounded number of loops; for an indefinitely-looping
 * pattern, use LookaheadPlayer instead so events aren't scheduled unboundedly far ahead. */
declare function compilePattern(pattern: Pattern, opts: CompileOptions): ScheduledEvent[];

declare class LookaheadPlayer {
    private readonly runtime;
    private pattern;
    private readonly stepDurationSeconds;
    private readonly loop;
    private readonly lookaheadSeconds;
    private readonly tickIntervalMs;
    private timerId;
    private nextStepIndex;
    private loopStartTime;
    constructor(runtime: AudioRuntime, pattern: Pattern, stepDurationSeconds: number, loop?: boolean, lookaheadSeconds?: number, tickIntervalMs?: number);
    start(startAt?: number): void;
    stop(): void;
    isPlaying(): boolean;
    /** Current step index for playhead visualization. Purely derived from the engine's own
     * clock (getCurrentTime()) — never the timer above — so a slow/throttled UI thread never
     * desyncs audio, only the visual playhead's redraw rate. Returns -1 before playback
     * has audibly started (e.g. during the initial scheduling lead-in). */
    getPlayheadStep(): number;
    private tick;
    private stepTime;
}

export { type CompileOptions, LookaheadPlayer, type Pattern, type PatternNote, compilePattern, compileStep };
