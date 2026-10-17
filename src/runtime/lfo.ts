import { LfoWaveform } from "./types";

// Pure, main-thread evaluation of the engine's LFO shape and timing model, for applications that
// need an LFO value at an *event time* (e.g. a sequencer modulating a note's velocity when it
// schedules the note) rather than continuously inside a synth. It mirrors native `dsp/lfo.h`
// exactly — same waveforms, same division table, same cycle/phase maths — and a parity check in
// native/test/smoke.mjs compares it against the compiled engine, so the two cannot drift apart.
// The engine's own LFOs (AudioRuntime.setInstrumentLfo) do not use this; see docs/lfo-modulation.md.

/** Sync divisions, slowest first (same order as `ModulationCapabilities.divisions`). `beats` is
 * the cycle length in quarter-note beats. */
export const LFO_DIVISIONS: readonly { name: string; beats: number }[] = [
  { name: "4 bars", beats: 16 },
  { name: "2 bars", beats: 8 },
  { name: "1/1", beats: 4 },
  { name: "1/2.", beats: 3 },
  { name: "1/2", beats: 2 },
  { name: "1/4.", beats: 1.5 },
  { name: "1/2T", beats: 4 / 3 },
  { name: "1/4", beats: 1 },
  { name: "1/8.", beats: 0.75 },
  { name: "1/4T", beats: 2 / 3 },
  { name: "1/8", beats: 0.5 },
  { name: "1/16.", beats: 0.375 },
  { name: "1/8T", beats: 1 / 3 },
  { name: "1/16", beats: 0.25 },
  { name: "1/16T", beats: 1 / 6 },
  { name: "1/32", beats: 0.125 },
];

/** Waveform value in [-1, 1] for a phase (any real; wrapped to [0, 1)). Sine and triangle start
 * at 0 and rise, saw ramps -1 -> +1, square is +1 for the first half. */
export function lfoShape(waveform: LfoWaveform, phase: number): number {
  const p = phase - Math.floor(phase);
  switch (waveform) {
    case LfoWaveform.Sine:
      return Math.sin(2 * Math.PI * p);
    case LfoWaveform.Triangle:
      return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
    case LfoWaveform.Square:
      return p < 0.5 ? 1 : -1;
    case LfoWaveform.Saw:
      return 2 * p - 1;
  }
  return 0;
}

export interface LfoEvalSettings {
  waveform: LfoWaveform;
  /** false: free-running at `rateHz`; true: locked to the beat grid at `division`. */
  sync: boolean;
  /** Free mode, Hz. */
  rateHz: number;
  /** Sync mode: index into LFO_DIVISIONS. */
  division: number;
  /** Phase offset, 0..1 (default 0). */
  phase?: number;
}

export interface LfoEvalContext {
  /** The absolute time to evaluate at, seconds (e.g. an event's scheduled time). */
  time: number;
  /** Time of the beat grid's anchor (AudioRuntime.setTempo's anchorTime). Cycle 0 starts here, in
   * both modes. */
  anchorTime: number;
  /** Tempo of the beat grid; a beat is a quarter note. Sync mode only. */
  bpm: number;
}

/** LFO output in [-1, 1] at `ctx.time`. A pure function of its arguments: the same settings and
 * context always give the same value, regardless of when it is called. */
export function lfoValueAt(s: LfoEvalSettings, ctx: LfoEvalContext): number {
  const elapsed = ctx.time - ctx.anchorTime;
  let cycles: number;
  if (s.sync) {
    const division = LFO_DIVISIONS[Math.min(LFO_DIVISIONS.length - 1, Math.max(0, Math.round(s.division)))];
    cycles = elapsed / ((60 / ctx.bpm) * division.beats);
  } else {
    cycles = elapsed * s.rateHz;
  }
  return lfoShape(s.waveform, cycles + (s.phase ?? 0));
}
