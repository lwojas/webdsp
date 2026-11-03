import { MatrixIn, MatrixOut, MatrixParam } from "./types";

// Pure, UI-free description of the Matrix synth's patch bay (InstrumentKind.Matrix) for clients: the outs
// and ins, how a cell maps onto an instrument parameter id, and a small analysis of a patch (which cables
// close a feedback loop, where a signal ends up) so an application can draw a matrix, a flow diagram or a
// compact list from the same parameters. The engine does not use this file and the synth does not need it;
// it reads the cells from the instrument's ordinary parameters. See docs/matrix-instrument.md.
//
// The voice is semi-modular, like a Mother-32: fixed modules joined only by cables, with the usual voice
// as the default patch. Outs run across the top of a matrix, ins down the side.

export const MATRIX_OUT_COUNT = 8;
export const MATRIX_IN_COUNT = 11;

/** What an out carries. Audio outs run at audio rate; control outs (LFOs, envelopes) are slow. */
export type MatrixSignal = "audio" | "control";
/** What an in does with what is sent to it: audio ins mix signals, control ins move a parameter, the output is the end. */
export type MatrixInRole = "audio" | "control" | "output";
/** What kind of cable a patch is: a control cable, an audio cable, or a feedback loop. */
export type MatrixPatchKind = "control" | "audio" | "feedback";

/** The modules a cable joins. `out` is the voice output, which has an in but no out. */
export type MatrixModule = "vco" | "noise" | "lfo1" | "lfo2" | "env1" | "env2" | "vcf" | "vca" | "out";

export interface MatrixOutInfo {
  out: MatrixOut;
  /** Stable key, the prefix of the cell's parameter name (`vcf_cutoff`). */
  key: string;
  name: string;
  signal: MatrixSignal;
  /** The module this out belongs to. */
  module: MatrixModule;
}

export interface MatrixInInfo {
  input: MatrixIn;
  /** Stable key, the suffix of the cell's parameter name. */
  key: string;
  name: string;
  role: MatrixInRole;
  /** The module this in belongs to. */
  module: MatrixModule;
  /** What a cable of amount 1 does at the signal's peak, in plain units, for tooltips and readouts. */
  fullScale: string;
}

export const MATRIX_OUTS: readonly MatrixOutInfo[] = [
  { out: MatrixOut.Vco, key: "vco", name: "VCO", signal: "audio", module: "vco" },
  { out: MatrixOut.Noise, key: "noise", name: "Noise", signal: "audio", module: "noise" },
  { out: MatrixOut.Lfo1, key: "lfo1", name: "LFO 1", signal: "control", module: "lfo1" },
  { out: MatrixOut.Lfo2, key: "lfo2", name: "LFO 2", signal: "control", module: "lfo2" },
  { out: MatrixOut.Env1, key: "env1", name: "ENV 1", signal: "control", module: "env1" },
  { out: MatrixOut.Env2, key: "env2", name: "ENV 2", signal: "control", module: "env2" },
  { out: MatrixOut.Vcf, key: "vcf", name: "VCF", signal: "audio", module: "vcf" },
  { out: MatrixOut.Vca, key: "vca", name: "VCA", signal: "audio", module: "vca" },
];

export const MATRIX_INS: readonly MatrixInInfo[] = [
  { input: MatrixIn.Pitch, key: "pitch", name: "VCO pitch", role: "control", module: "vco", fullScale: "1 octave" },
  { input: MatrixIn.Pwm, key: "pwm", name: "VCO pulse width", role: "control", module: "vco", fullScale: "±0.45 of the cycle" },
  { input: MatrixIn.VcfIn, key: "vcfin", name: "VCF in", role: "audio", module: "vcf", fullScale: "unity gain" },
  { input: MatrixIn.Cutoff, key: "cutoff", name: "VCF cutoff", role: "control", module: "vcf", fullScale: "5 octaves" },
  { input: MatrixIn.Resonance, key: "resonance", name: "VCF resonance", role: "control", module: "vcf", fullScale: "±0.5" },
  { input: MatrixIn.Drive, key: "drive", name: "VCF drive", role: "control", module: "vcf", fullScale: "±0.6" },
  { input: MatrixIn.VcaIn, key: "vcain", name: "VCA in", role: "audio", module: "vca", fullScale: "unity gain" },
  { input: MatrixIn.Level, key: "level", name: "VCA level", role: "control", module: "vca", fullScale: "full level" },
  { input: MatrixIn.Lfo1Rate, key: "lfo1rate", name: "LFO 1 rate", role: "control", module: "lfo1", fullScale: "3 octaves" },
  { input: MatrixIn.Lfo2Rate, key: "lfo2rate", name: "LFO 2 rate", role: "control", module: "lfo2", fullScale: "3 octaves" },
  { input: MatrixIn.Out, key: "out", name: "Output", role: "output", module: "out", fullScale: "unity gain" },
];

/** One cable: carry `out` into `input`, scaled by `amount` (-1..1). */
export interface MatrixPatch {
  out: MatrixOut;
  input: MatrixIn;
  amount: number;
}

/** Instrument parameter id of a cell. Pass it to `setInstrumentParameter` (or schedule it). */
export function matrixAmountParam(out: MatrixOut, input: MatrixIn): number {
  return MatrixParam.AmountBase + out * MATRIX_IN_COUNT + input;
}

/** The (out, in) a cell's parameter id addresses, or null for any other id. */
export function matrixCellOfParam(param: number): { out: MatrixOut; input: MatrixIn } | null {
  const i = param - MatrixParam.AmountBase;
  if (!Number.isInteger(i) || i < 0 || i >= MATRIX_OUT_COUNT * MATRIX_IN_COUNT) return null;
  return { out: Math.floor(i / MATRIX_IN_COUNT), input: i % MATRIX_IN_COUNT };
}

/** Every cable in use (amount other than 0), in matrix order. `read` returns a parameter's current value,
 * for example `(id) => values.get(id) ?? defaults.get(id) ?? 0` over what the client last set. */
export function matrixPatches(read: (param: number) => number): MatrixPatch[] {
  const out: MatrixPatch[] = [];
  for (const o of MATRIX_OUTS) {
    for (const i of MATRIX_INS) {
      const amount = read(matrixAmountParam(o.out, i.input));
      if (Number.isFinite(amount) && amount !== 0) out.push({ out: o.out, input: i.input, amount });
    }
  }
  return out;
}

type Edge = [MatrixModule, MatrixModule];
const edges = (patches: readonly MatrixPatch[]): Edge[] => patches.map((p) => [MATRIX_OUTS[p.out].module, MATRIX_INS[p.input].module]);

function route(from: MatrixModule, to: MatrixModule, e: readonly Edge[]): MatrixModule[] | null {
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

/** The modules a cable closes a loop through, starting and ending at the module the signal is taken from
 * (`["vcf","vcf"]` for VCF out into its own cutoff; `["vcf","vco","vcf"]` for VCF out into pitch with the
 * oscillator patched into the filter), or null if the cable is not part of a loop. `others` are the rest of
 * the patch: a loop closes through them, so one cell can be feedback in one patch and not in another. */
export function matrixLoop(patch: MatrixPatch, others: readonly MatrixPatch[] = []): MatrixModule[] | null {
  const from = MATRIX_OUTS[patch.out].module;
  const to = MATRIX_INS[patch.input].module;
  const path = route(to, from, edges(others));
  return path ? [from, ...path] : null;
}

/** Whether patching (out, in) on top of `patches` would close a loop. Use it to colour empty cells, so
 * feedback is visible before it is made. */
export function matrixWouldFeedback(out: MatrixOut, input: MatrixIn, patches: readonly MatrixPatch[] = []): boolean {
  const others = patches.filter((p) => !(p.out === out && p.input === input));
  return matrixLoop({ out, input, amount: 1 }, others) !== null;
}

/** Control, audio or feedback. Feedback wins: a loop is a loop whatever drives it. */
export function matrixPatchKind(patch: MatrixPatch, all: readonly MatrixPatch[] = []): MatrixPatchKind {
  const others = all.filter((p) => !(p.out === patch.out && p.input === patch.input));
  if (matrixLoop(patch, others) !== null) return "feedback";
  return MATRIX_OUTS[patch.out].signal === "audio" ? "audio" : "control";
}

/** The way a signal sent into `input` reaches the voice output through the patch: the modules from the
 * in's own module onward (`Cutoff` -> `["vcf", "vca", "out"]` in the default patch), or null when nothing
 * connects it to the output (the cable goes nowhere you can hear). */
export function matrixPath(input: MatrixIn, patches: readonly MatrixPatch[]): MatrixModule[] | null {
  return route(MATRIX_INS[input].module, "out", edges(patches));
}
