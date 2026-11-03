import { describe, expect, it } from "vitest";
import {
  MATRIX_INS, MATRIX_IN_COUNT, MATRIX_OUTS, MATRIX_OUT_COUNT, MatrixIn, MatrixOut, MatrixParam, matrixAmountParam,
  matrixCellOfParam, matrixLoop, matrixPatchKind, matrixPatches, matrixPath, matrixWouldFeedback, type MatrixPatch,
} from "../src/runtime";

const p = (out: MatrixOut, input: MatrixIn, amount = 0.5): MatrixPatch => ({ out, input, amount });
// The engine's default patch (native/src/instruments/matrix.h): the ordinary voice, normalled.
const NORMAL = [
  p(MatrixOut.Vco, MatrixIn.VcfIn, 1), p(MatrixOut.Env1, MatrixIn.Level, 1), p(MatrixOut.Env2, MatrixIn.Cutoff, 0.4),
  p(MatrixOut.Vcf, MatrixIn.VcaIn, 1), p(MatrixOut.Vca, MatrixIn.Out, 1),
];

describe("Matrix cell ids (client side of native params.h)", () => {
  it("pins the formula and the corners of the matrix", () => {
    expect(matrixAmountParam(MatrixOut.Vco, MatrixIn.Pitch)).toBe(21);
    expect(matrixAmountParam(MatrixOut.Env1, MatrixIn.Level)).toBe(21 + 4 * 11 + 7);
    expect(matrixAmountParam(MatrixOut.Vca, MatrixIn.Out)).toBe(108);
    expect(MATRIX_OUT_COUNT * MATRIX_IN_COUNT).toBe(88);
  });

  it("round-trips every cell and rejects other ids", () => {
    for (const o of MATRIX_OUTS) {
      for (const i of MATRIX_INS) {
        expect(matrixCellOfParam(matrixAmountParam(o.out, i.input))).toEqual({ out: o.out, input: i.input });
      }
    }
    expect(matrixCellOfParam(MatrixParam.Env2Release)).toBeNull();
    expect(matrixCellOfParam(109)).toBeNull();
    expect(matrixCellOfParam(1000)).toBeNull();
  });

  it("describes outs and ins in id order, with the native parameter names", () => {
    expect(MATRIX_OUTS.map((o) => o.out)).toEqual([0, 1, 2, 3, 4, 5, 6, 7]);
    expect(MATRIX_INS.map((i) => i.input)).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    expect(`${MATRIX_OUTS[MatrixOut.Vcf].key}_${MATRIX_INS[MatrixIn.Cutoff].key}`).toBe("vcf_cutoff");
    expect(`${MATRIX_OUTS[MatrixOut.Vca].key}_${MATRIX_INS[MatrixIn.Out].key}`).toBe("vca_out");
    expect(MATRIX_OUTS.filter((o) => o.signal === "audio").map((o) => o.key)).toEqual(["vco", "noise", "vcf", "vca"]);
    expect(MATRIX_INS.filter((i) => i.role === "audio").map((i) => i.key)).toEqual(["vcfin", "vcain"]);
  });
});

describe("matrixPatches", () => {
  it("lists only cables in use, in matrix order, ignoring junk", () => {
    const values = new Map<number, number>([
      [matrixAmountParam(MatrixOut.Env1, MatrixIn.Level), 1],
      [matrixAmountParam(MatrixOut.Lfo1, MatrixIn.Cutoff), -0.4],
      [matrixAmountParam(MatrixOut.Noise, MatrixIn.Pwm), 0],
      [matrixAmountParam(MatrixOut.Vco, MatrixIn.Pitch), Number.NaN],
    ]);
    expect(matrixPatches((id) => values.get(id) ?? 0)).toEqual([
      p(MatrixOut.Lfo1, MatrixIn.Cutoff, -0.4),
      p(MatrixOut.Env1, MatrixIn.Level, 1),
    ]);
  });
});

describe("feedback analysis", () => {
  it("the normal wiring has no loop", () => {
    for (const patch of NORMAL) expect(matrixLoop(patch, NORMAL)).toBeNull();
    expect(matrixPatchKind(p(MatrixOut.Vco, MatrixIn.VcfIn), NORMAL)).toBe("audio");
    expect(matrixPatchKind(p(MatrixOut.Env2, MatrixIn.Cutoff), NORMAL)).toBe("control");
  });

  it("anything carried back to where the signal came from is feedback, with the loop it closes", () => {
    expect(matrixLoop(p(MatrixOut.Vcf, MatrixIn.Cutoff), NORMAL)).toEqual(["vcf", "vcf"]);
    expect(matrixLoop(p(MatrixOut.Vcf, MatrixIn.VcfIn), NORMAL)).toEqual(["vcf", "vcf"]);
    expect(matrixLoop(p(MatrixOut.Vco, MatrixIn.Pitch), NORMAL)).toEqual(["vco", "vco"]);
    expect(matrixLoop(p(MatrixOut.Vcf, MatrixIn.Pitch), NORMAL)).toEqual(["vcf", "vco", "vcf"]);
    expect(matrixLoop(p(MatrixOut.Vca, MatrixIn.Cutoff), NORMAL)).toEqual(["vca", "vcf", "vca"]);
    expect(matrixLoop(p(MatrixOut.Vca, MatrixIn.Pitch), NORMAL)).toEqual(["vca", "vco", "vcf", "vca"]);
    expect(matrixPatchKind(p(MatrixOut.Vcf, MatrixIn.Resonance), NORMAL)).toBe("feedback");
  });

  it("depends on the rest of the patch: a loop closes only through cables that are there", () => {
    // With the oscillator not patched into the filter, filter out into pitch is not a loop.
    const rewired = NORMAL.filter((x) => x.out !== MatrixOut.Vco);
    expect(matrixLoop(p(MatrixOut.Vcf, MatrixIn.Pitch), rewired)).toBeNull();
    // The oscillator straight into the amp: the amp's output into the oscillator's pitch is a loop again.
    const bypass = [...rewired, p(MatrixOut.Vco, MatrixIn.VcaIn)];
    expect(matrixLoop(p(MatrixOut.Vca, MatrixIn.Pitch), bypass)).toEqual(["vca", "vco", "vca"]);
    expect(matrixPatchKind(p(MatrixOut.Vco, MatrixIn.Cutoff), [...NORMAL, p(MatrixOut.Vcf, MatrixIn.Pitch)])).toBe("feedback");
  });

  it("predicts feedback for an empty cell, to colour it before it is patched", () => {
    expect(matrixWouldFeedback(MatrixOut.Vcf, MatrixIn.Cutoff, NORMAL)).toBe(true);
    expect(matrixWouldFeedback(MatrixOut.Lfo2, MatrixIn.Cutoff, NORMAL)).toBe(false);
    expect(matrixWouldFeedback(MatrixOut.Noise, MatrixIn.VcfIn, NORMAL)).toBe(false);
    expect(matrixWouldFeedback(MatrixOut.Vca, MatrixIn.VcfIn, NORMAL)).toBe(true);
    // A cell that is already patched is judged without itself.
    expect(matrixWouldFeedback(MatrixOut.Vco, MatrixIn.VcfIn, NORMAL)).toBe(false);
  });
});

describe("matrixPath", () => {
  it("follows the cables to the output, and says when there are none", () => {
    expect(matrixPath(MatrixIn.Cutoff, NORMAL)).toEqual(["vcf", "vca", "out"]);
    expect(matrixPath(MatrixIn.Pitch, NORMAL)).toEqual(["vco", "vcf", "vca", "out"]);
    expect(matrixPath(MatrixIn.Level, NORMAL)).toEqual(["vca", "out"]);
    expect(matrixPath(MatrixIn.Out, NORMAL)).toEqual(["out"]);
    expect(matrixPath(MatrixIn.Lfo1Rate, NORMAL)).toBeNull();   // LFO 1 is not patched anywhere
    expect(matrixPath(MatrixIn.Cutoff, NORMAL.filter((x) => x.out !== MatrixOut.Vcf))).toBeNull();   // the filter goes nowhere
  });
});
