// A native sequence: the song as a list of timed events the HOST plays against its own transport, so it keeps playing without the page.
//
// An application that sequences in the page (a JS timer scheduling a window ahead) stops when the page does: a plugin's WebView is destroyed with
// its window, and it cannot keep up with a DAW's offline bounce. A Sequence is the page handing the host, ahead of time, one cycle of the song in
// musical time. The host plays it on its audio thread, locked to the DAW's transport (position, tempo, loops and locates, offline bounce
// included), with the window open or closed. The page stays the editor: every edit sends the whole sequence again and the host swaps it in
// at the next block. See docs/native-sequence.md.
//
// Times are in quarter notes (the host's PPQ): `beat` 0 is the start of the DAW's timeline, and the sequence repeats every `lengthBeats`, so
// a song that loops plays at DAW position p what the sequence has at `p mod lengthBeats`. Tempo is the DAW's: durations are in beats too.
import type { BusId, InstrumentId, SampleId, ScheduledParamTarget } from "./types";

export interface SequenceSampleEvent {
  beat: number;
  kind: "sample";
  sample: SampleId;
  bus?: BusId;
  /** Level, 0..1+ (default 1). */
  gain?: number;
  /** -1..+1 (default 0). */
  pan?: number;
  /** Semitones from the sample's own pitch. */
  pitch?: number;
  /** A cue region of the sample (0 = the first), which then never transposes. */
  region?: number;
  loop?: boolean;
  /** Releases after this many beats; omit to play to the end (or until released). */
  durationBeats?: number;
  chokeGroup?: number;
  /** With `mono`, a new event on the same `track` releases the previous voice of that track, at this event's start. */
  track?: number;
  mono?: boolean;
}

export interface SequenceInstrumentEvent {
  beat: number;
  kind: "instrument";
  instrument: InstrumentId;
  /** Level, 0..1 (default 1). */
  velocity?: number;
  pan?: number;
  pitch?: number;
  /** A sustained synth is released after this many beats; omit for a one-shot (drums end themselves). */
  durationBeats?: number;
  chokeGroup?: number;
  /** Tied to the note still held (InstrumentNoteParams.tie). Older hosts ignore it. */
  tie?: boolean;
}

/** A write of a parameter's base value (a step, or a ramp), as AudioRuntime.scheduleParams() but in beats. */
export interface SequenceParamEvent {
  beat: number;
  kind: "param";
  target: ScheduledParamTarget;
  param: number;
  value: number;
  from?: number;
  rampBeats?: number;
}

export type SequenceEvent = SequenceSampleEvent | SequenceInstrumentEvent | SequenceParamEvent;

export interface Sequence {
  /** One cycle of the song, in quarter notes (> 0). The sequence repeats every `lengthBeats`. */
  lengthBeats: number;
  events: SequenceEvent[];
}

/** The most events a host will take in one sequence. */
export const MAX_SEQUENCE_EVENTS = 200_000;

const finite = (n: unknown): n is number => typeof n === "number" && Number.isFinite(n);

export interface SequenceKnown {
  sample(id: number): boolean;
  instrument(id: number): boolean;
  effect(bus: number, effect: number): boolean;
  /** A bus whose mixer parameters a param event may write (host protocol 4). */
  bus?(bus: number): boolean;
}

/** The sequence cleaned up for the wire: unusable events dropped (an unknown sample, instrument or effect; non-finite numbers; a beat outside
 * the cycle), numbers clamped, events in time order. Null when there is nothing playable (no length, no events). */
export function normalizeSequence(seq: Sequence | null | undefined, known: SequenceKnown): Sequence | null {
  if (!seq || !finite(seq.lengthBeats) || seq.lengthBeats <= 0 || !Array.isArray(seq.events)) return null;
  const length = seq.lengthBeats;
  const events: SequenceEvent[] = [];

  for (const e of seq.events) {
    if (events.length >= MAX_SEQUENCE_EVENTS) break;
    if (!e || !finite(e.beat) || e.beat < 0 || e.beat >= length) continue;
    const dur = (v: unknown) => (finite(v) && v > 0 ? { durationBeats: v } : {});
    const choke = (v: unknown) => (finite(v) && v >= 1 ? { chokeGroup: Math.floor(v) } : {});
    const num = (key: string, v: unknown) => (finite(v) ? { [key]: v } : {});

    if (e.kind === "sample") {
      if (!finite(e.sample) || !known.sample(e.sample)) continue;
      events.push({
        beat: e.beat,
        kind: "sample",
        sample: e.sample,
        ...(finite(e.bus) && e.bus >= 0 ? { bus: Math.floor(e.bus) } : {}),
        ...num("gain", e.gain),
        ...(finite(e.pan) ? { pan: Math.min(1, Math.max(-1, e.pan)) } : {}),
        ...num("pitch", e.pitch),
        ...(finite(e.region) && e.region >= 0 ? { region: Math.floor(e.region) } : {}),
        ...(e.loop ? { loop: true } : {}),
        ...dur(e.durationBeats),
        ...choke(e.chokeGroup),
        ...(finite(e.track) && e.track >= 0 ? { track: Math.floor(e.track) } : {}),
        ...(e.mono ? { mono: true } : {}),
      });
    } else if (e.kind === "instrument") {
      if (!finite(e.instrument) || !known.instrument(e.instrument)) continue;
      events.push({
        beat: e.beat,
        kind: "instrument",
        instrument: e.instrument,
        ...(finite(e.velocity) ? { velocity: Math.min(1, Math.max(0, e.velocity)) } : {}),
        ...(finite(e.pan) ? { pan: Math.min(1, Math.max(-1, e.pan)) } : {}),
        ...num("pitch", e.pitch),
        ...dur(e.durationBeats),
        ...choke(e.chokeGroup),
        ...(e.tie ? { tie: true } : {}),
      });
    } else if (e.kind === "param") {
      const t = e.target;
      if (!finite(e.param) || !finite(e.value) || !t) continue;
      if ("effect" in t) {
        if (!known.effect(t.effect.bus, t.effect.effect)) continue;
      } else if ("instrument" in t) {
        if (!known.instrument(t.instrument)) continue;
      } else if ("sample" in t) {
        if (!known.sample(t.sample)) continue;
      } else if ("bus" in t) {
        if (!known.bus?.(t.bus)) continue;
      } else continue;
      events.push({
        beat: e.beat,
        kind: "param",
        target: t,
        param: e.param,
        value: e.value,
        ...num("from", e.from),
        ...(finite(e.rampBeats) && e.rampBeats > 0 ? { rampBeats: e.rampBeats } : {}),
      });
    }
  }
  if (events.length === 0) return null;
  events.sort((a, b) => a.beat - b.beat); // stable: equal beats keep the application's order
  return { lengthBeats: length, events };
}
