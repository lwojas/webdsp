import { MatrixDest, MatrixParam, MatrixSource } from "./types";

// Pure, UI-free description of the Matrix synth's patch matrix (InstrumentKind.Matrix) for
// clients: the sources and destinations, how a cell maps onto an instrument parameter id, and a
// small analysis of a patch (which connections close a feedback loop, where a signal ends up) so an
// application can draw a matrix, a flow diagram or a compact chip list from the same parameters.
// The engine does not use this file and the synth does not need it; it reads the cells from the
// instrument's ordinary parameters. See docs/matrix-instrument.md.

export const MATRIX_SOURCE_COUNT = 7;
export const MATRIX_DEST_COUNT = 6;

/** What a source carries. Control sources move slowly (LFOs, envelopes); audio sources run at audio rate. */
export type MatrixSignal = "control" | "audio";

/** What kind of connection a patch is: a control patch, an audio-rate patch, or a feedback loop. */
export type MatrixPatchKind = "control" | "audio" | "feedback";

export interface MatrixSourceInfo {
  source: MatrixSource;
  /** Stable key, the prefix of the cell's parameter name (`lfo1_cutoff`). */
  key: string;
  name: string;
  signal: MatrixSignal;
  /** The module the signal is taken from (see `MatrixModule`). */
  module: MatrixModule;
}

export interface MatrixDestInfo {
  dest: MatrixDest;
  /** Stable key, the suffix of the cell's parameter name. */
  key: string;
  name: string;
  /** The module the destination belongs to. */
  module: MatrixModule;
  /** What a 100% patch does, in plain units: for tooltips and readouts. */
  fullScale: string;
}

/** The blocks of the voice a signal path runs through. */
export type MatrixModule = "lfo1" | "lfo2" | "env1" | "env2" | "noise" | "vco" | "vcf" | "vca" | "out";

export const MATRIX_SOURCES: readonly MatrixSourceInfo[] = [
  { source: MatrixSource.Lfo1, key: "lfo1", name: "LFO 1", signal: "control", module: "lfo1" },
  { source: MatrixSource.Lfo2, key: "lfo2", name: "LFO 2", signal: "control", module: "lfo2" },
  { source: MatrixSource.Env1, key: "env1", name: "ENV 1", signal: "control", module: "env1" },
  { source: MatrixSource.Env2, key: "env2", name: "ENV 2", signal: "control", module: "env2" },
  { source: MatrixSource.Noise, key: "noise", name: "Noise", signal: "audio", module: "noise" },
  { source: MatrixSource.Vco, key: "vco", name: "VCO out", signal: "audio", module: "vco" },
  { source: MatrixSource.Vcf, key: "vcf", name: "VCF out", signal: "audio", module: "vcf" },
];

export const MATRIX_DESTS: readonly MatrixDestInfo[] = [
  { dest: MatrixDest.Pitch, key: "pitch", name: "Pitch", module: "vco", fullScale: "1 octave" },
  { dest: MatrixDest.Pwm, key: "pwm", name: "Pulse width", module: "vco", fullScale: "±0.45 of the cycle" },
  { dest: MatrixDest.Cutoff, key: "cutoff", name: "Cutoff", module: "vcf", fullScale: "5 octaves" },
  { dest: MatrixDest.Resonance, key: "resonance", name: "Resonance", module: "vcf", fullScale: "±0.5" },
  { dest: MatrixDest.Drive, key: "drive", name: "Drive", module: "vcf", fullScale: "±0.6" },
  { dest: MatrixDest.Level, key: "level", name: "Level", module: "vca", fullScale: "full level" },
];

/** The fixed audio path: each module feeds the next. Noise joins at the filter input. */
const NEXT: Partial<Record<MatrixModule, MatrixModule>> = { vco: "vcf", noise: "vcf", vcf: "vca", vca: "out" };

/** One cell of the matrix: carry `source` to `dest`, scaled by `amount` (-1..1). */
export interface MatrixPatch {
  source: MatrixSource;
  dest: MatrixDest;
  amount: number;
}

/** Instrument parameter id of a cell. Pass it to `setInstrumentParameter` (or schedule it). */
export function matrixAmountParam(source: MatrixSource, dest: MatrixDest): number {
  return MatrixParam.AmountBase + source * MATRIX_DEST_COUNT + dest;
}

/** The (source, destination) a cell's parameter id addresses, or null for any other id. */
export function matrixCellOfParam(param: number): { source: MatrixSource; dest: MatrixDest } | null {
  const i = param - MatrixParam.AmountBase;
  if (!Number.isInteger(i) || i < 0 || i >= MATRIX_SOURCE_COUNT * MATRIX_DEST_COUNT) return null;
  return { source: Math.floor(i / MATRIX_DEST_COUNT), dest: i % MATRIX_DEST_COUNT };
}

/** Every cell that is in use (amount other than 0), in matrix order. `read` returns a parameter's
 * current value, for example `(id) => values.get(id) ?? 0` over what the client last set. */
export function matrixPatches(read: (param: number) => number): MatrixPatch[] {
  const out: MatrixPatch[] = [];
  for (const s of MATRIX_SOURCES) {
    for (const d of MATRIX_DESTS) {
      const amount = read(matrixAmountParam(s.source, d.dest));
      if (Number.isFinite(amount) && amount !== 0) out.push({ source: s.source, dest: d.dest, amount });
    }
  }
  return out;
}

function edges(patches: readonly MatrixPatch[]): [MatrixModule, MatrixModule][] {
  const e: [MatrixModule, MatrixModule][] = [];
  for (const [from, to] of Object.entries(NEXT)) e.push([from as MatrixModule, to as MatrixModule]);
  for (const p of patches) e.push([MATRIX_SOURCES[p.source].module, MATRIX_DESTS[p.dest].module]);
  return e;
}

function route(from: MatrixModule, to: MatrixModule, e: [MatrixModule, MatrixModule][]): MatrixModule[] | null {
  if (from === to) return [from];
  const prev = new Map<MatrixModule, MatrixModule>();
  const queue: MatrixModule[] = [from];
  while (queue.length) {
    const n = queue.shift() as MatrixModule;
    for (const [a, b] of e) {
      if (a !== n || b === from || prev.has(b)) continue;
      prev.set(b, n);
      if (b === to) {
        const path: MatrixModule[] = [to];
        for (let x = n; x !== from; x = prev.get(x) as MatrixModule) path.unshift(x);
        path.unshift(from);
        return path;
      }
      queue.push(b);
    }
  }
  return null;
}

/** The modules a patch closes a loop through, starting and ending at the module the signal is taken
 * from (`["vcf", "vcf"]` for VCF out to cutoff; `["vcf", "vco", "vcf"]` for VCF out to pitch), or
 * null if the patch is not feedback. `others` are the rest of the patch: a loop may close through
 * them, so the same cell can be feedback in one patch and not in another. */
export function matrixLoop(patch: MatrixPatch, others: readonly MatrixPatch[] = []): MatrixModule[] | null {
  const from = MATRIX_SOURCES[patch.source].module;
  const to = MATRIX_DESTS[patch.dest].module;
  const path = route(to, from, edges(others));
  return path ? [from, ...path] : null;
}

/** Whether patching (source, dest) on top of `patches` would close a loop. Use it to colour empty
 * cells, so feedback is visible before it is made. */
export function matrixWouldFeedback(source: MatrixSource, dest: MatrixDest, patches: readonly MatrixPatch[] = []): boolean {
  return matrixLoop({ source, dest, amount: 1 }, patches) !== null;
}

/** Control, audio-rate or feedback. Feedback wins: a loop is a loop whatever drives it. */
export function matrixPatchKind(patch: MatrixPatch, all: readonly MatrixPatch[] = []): MatrixPatchKind {
  if (matrixLoop(patch, all) !== null) return "feedback";
  return MATRIX_SOURCES[patch.source].signal === "audio" ? "audio" : "control";
}

/** The modules a destination's output goes on through to the output, after its own module
 * (`Cutoff` -> `["vca", "out"]`), for answering "where is this signal going". */
export function matrixDownstream(dest: MatrixDest): MatrixModule[] {
  const out: MatrixModule[] = [];
  for (let m = NEXT[MATRIX_DESTS[dest].module]; m; m = NEXT[m]) out.push(m);
  return out;
}
