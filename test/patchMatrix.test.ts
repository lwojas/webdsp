import { describe, expect, it } from "vitest";
import {
  MATRIX_DESTS, MATRIX_SOURCES, MATRIX_DEST_COUNT, MATRIX_SOURCE_COUNT, MatrixDest, MatrixParam,
  MatrixSource, matrixAmountParam, matrixCellOfParam, matrixDownstream, matrixLoop, matrixPatchKind,
  matrixPatches, matrixWouldFeedback, type MatrixPatch,
} from "../src/runtime";

const p = (source: MatrixSource, dest: MatrixDest, amount = 0.5): MatrixPatch => ({ source, dest, amount });

describe("Matrix cell ids (client side of native params.h)", () => {
  it("pins the formula and the corners of the matrix", () => {
    expect(matrixAmountParam(MatrixSource.Lfo1, MatrixDest.Pitch)).toBe(22);
    expect(matrixAmountParam(MatrixSource.Env1, MatrixDest.Level)).toBe(39);
    expect(matrixAmountParam(MatrixSource.Env2, MatrixDest.Cutoff)).toBe(42);
    expect(matrixAmountParam(MatrixSource.Vcf, MatrixDest.Cutoff)).toBe(22 + 6 * 6 + 2);
    expect(matrixAmountParam(MatrixSource.Vcf, MatrixDest.Level)).toBe(63);
    expect(MATRIX_SOURCE_COUNT * MATRIX_DEST_COUNT).toBe(42);
  });

  it("round-trips every cell and rejects other ids", () => {
    for (const s of MATRIX_SOURCES) {
      for (const d of MATRIX_DESTS) {
        expect(matrixCellOfParam(matrixAmountParam(s.source, d.dest))).toEqual({ source: s.source, dest: d.dest });
      }
    }
    expect(matrixCellOfParam(MatrixParam.Env2Release)).toBeNull();
    expect(matrixCellOfParam(64)).toBeNull();
    expect(matrixCellOfParam(1000)).toBeNull();
  });

  it("describes sources and destinations in id order", () => {
    expect(MATRIX_SOURCES.map((s) => s.source)).toEqual([0, 1, 2, 3, 4, 5, 6]);
    expect(MATRIX_DESTS.map((d) => d.dest)).toEqual([0, 1, 2, 3, 4, 5]);
    // The keys are the native parameter names (native/test/matrix_test.cpp checks the other end).
    expect(`${MATRIX_SOURCES[MatrixSource.Vcf].key}_${MATRIX_DESTS[MatrixDest.Cutoff].key}`).toBe("vcf_cutoff");
    expect(MATRIX_SOURCES.filter((s) => s.signal === "audio").map((s) => s.key)).toEqual(["noise", "vco", "vcf"]);
  });
});

describe("matrixPatches", () => {
  it("lists only cells in use, in matrix order, ignoring junk", () => {
    const values = new Map<number, number>([
      [matrixAmountParam(MatrixSource.Env1, MatrixDest.Level), 1],
      [matrixAmountParam(MatrixSource.Lfo1, MatrixDest.Cutoff), -0.4],
      [matrixAmountParam(MatrixSource.Noise, MatrixDest.Pwm), 0],
      [matrixAmountParam(MatrixSource.Vco, MatrixDest.Pitch), Number.NaN],
    ]);
    expect(matrixPatches((id) => values.get(id) ?? 0)).toEqual([
      p(MatrixSource.Lfo1, MatrixDest.Cutoff, -0.4),
      p(MatrixSource.Env1, MatrixDest.Level, 1),
    ]);
  });
});

describe("feedback analysis", () => {
  it("control and audio-rate patches that go downstream are not feedback", () => {
    expect(matrixLoop(p(MatrixSource.Lfo1, MatrixDest.Cutoff))).toBeNull();
    expect(matrixLoop(p(MatrixSource.Env1, MatrixDest.Level))).toBeNull();
    expect(matrixLoop(p(MatrixSource.Vco, MatrixDest.Cutoff))).toBeNull();      // oscillator into the filter
    expect(matrixLoop(p(MatrixSource.Noise, MatrixDest.Cutoff))).toBeNull();
    expect(matrixLoop(p(MatrixSource.Vcf, MatrixDest.Level))).toBeNull();       // filter into the amp
    expect(matrixPatchKind(p(MatrixSource.Lfo1, MatrixDest.Cutoff))).toBe("control");
    expect(matrixPatchKind(p(MatrixSource.Vco, MatrixDest.Cutoff))).toBe("audio");
  });

  it("anything carried back upstream is feedback, with the loop it closes", () => {
    expect(matrixLoop(p(MatrixSource.Vcf, MatrixDest.Cutoff))).toEqual(["vcf", "vcf"]);
    expect(matrixLoop(p(MatrixSource.Vcf, MatrixDest.Pitch))).toEqual(["vcf", "vco", "vcf"]);
    expect(matrixLoop(p(MatrixSource.Vco, MatrixDest.Pitch))).toEqual(["vco", "vco"]);
    expect(matrixPatchKind(p(MatrixSource.Vcf, MatrixDest.Resonance))).toBe("feedback");
    expect(matrixPatchKind(p(MatrixSource.Vcf, MatrixDest.Pwm))).toBe("feedback");
  });

  it("predicts feedback for an empty cell, to colour it before it is patched", () => {
    expect(matrixWouldFeedback(MatrixSource.Vcf, MatrixDest.Cutoff)).toBe(true);
    expect(matrixWouldFeedback(MatrixSource.Lfo2, MatrixDest.Cutoff)).toBe(false);
    expect(matrixWouldFeedback(MatrixSource.Vcf, MatrixDest.Level)).toBe(false);
  });

  it("knows the path to the output", () => {
    expect(matrixDownstream(MatrixDest.Pitch)).toEqual(["vcf", "vca", "out"]);
    expect(matrixDownstream(MatrixDest.Cutoff)).toEqual(["vca", "out"]);
    expect(matrixDownstream(MatrixDest.Level)).toEqual(["out"]);
  });
});
