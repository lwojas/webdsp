# Matrix synth: guide for clients building a patching UI

The Matrix synth ([matrix-instrument.md](./matrix-instrument.md)) has no patching UI of its own. Its
patch matrix is 42 instrument parameters, and a client decides how to show and edit them. This guide
covers what the engine and `webdsp` give you and what a UI needs to decide.

## What you get

1. **A catalogue entry.** `InstrumentKind.Matrix` (id `"matrix"`, category `"synth"`) appears in the
   instrument catalogue from `ready` like every other kind, with all 64 controls and the shared
   `gain` row listed by `runtime.getInstrumentParams(InstrumentKind.Matrix)` (ranges, defaults,
   units, modulation domain).
2. **Cells as parameters.** Each cell is a normal parameter named `<source>_<dest>` (`lfo1_cutoff`,
   `vcf_pitch`), range -1..1, default 0 except the two init patches. Set one with
   `setInstrumentParameter`; automate one like any parameter.
3. **A model.** `import { ... } from "webdsp"`:

| Export | Use |
|---|---|
| `MatrixParam`, `MatrixSource`, `MatrixDest` | Ids of the voice controls, sources, destinations. |
| `MATRIX_SOURCES`, `MATRIX_DESTS` | Names, stable keys, signal type (control or audio), the module each belongs to, what 100% means. |
| `matrixAmountParam(source, dest)` | Parameter id of a cell. `matrixCellOfParam(id)` is the inverse. |
| `matrixPatches(read)` | The cells in use, from any value store. |
| `matrixLoop(patch, others)` | The loop a patch closes (`["vcf","vcf"]`), or null. |
| `matrixWouldFeedback(source, dest, patches)` | Colour an empty cell before it is patched. |
| `matrixPatchKind(patch, all)` | `"control"`, `"audio"` or `"feedback"`. |
| `matrixDownstream(dest)` | Where a destination's signal goes on to (`["vca","out"]`), for "where is this going". |

```ts
import { InstrumentKind, MatrixSource, MatrixDest, MatrixParam, matrixAmountParam, matrixPatches, matrixPatchKind } from "webdsp";

const synth = runtime.createInstrument(InstrumentKind.Matrix, { bus });
runtime.setInstrumentParameter(synth, MatrixParam.Cutoff, 600);
runtime.setInstrumentParameter(synth, MatrixParam.Resonance, 0.5);

// LFO 1 -> cutoff, 55%
runtime.setInstrumentParameter(synth, matrixAmountParam(MatrixSource.Lfo1, MatrixDest.Cutoff), 0.55);
// Filter output back into the filter cutoff: feedback
runtime.setInstrumentParameter(synth, matrixAmountParam(MatrixSource.Vcf, MatrixDest.Cutoff), 0.4);

const patches = matrixPatches((id) => mine.get(id) ?? 0);           // your own value store
patches.map((p) => matrixPatchKind(p, patches));                     // ["control", "feedback"]
```

The runtime does not read instrument parameter values back, so keep the values you set (or the
catalogue defaults) in your own state and feed `matrixPatches` from it.

## Rules of thumb

- **A patch is source, amount, destination.** Removing one is writing 0. There is nothing else to
  store, so a patch fits in a project file as the list `matrixPatches` returns.
- **Treat the matrix as one graph, not 42 knobs.** The same list drives every view: a grid (a cell
  per pair), a flow diagram (a line per patch, drawn from the module it starts at to the parameter it
  ends at), a synth panel (a ring on each knob showing how far patches move it), or a few chips in a
  small editor. `matrixPatches` is the one piece of state they all read.
- **Show feedback as feedback.** Use `matrixLoop`: it knows that VCF out to cutoff is a loop and that
  VCF out to level is not. Colour empty cells with `matrixWouldFeedback` so a user sees what a click
  would do before making it, and explain a loop in plain words ("the filter hears its own output").
  The engine already keeps it bounded, so there is no warning to raise; the colour is for
  understanding, not alarm.
- **Say what 100% means.** Amounts are normalised, so a UI should translate: `fullScale` in
  `MATRIX_DESTS` gives the unit (1 octave on pitch, 5 octaves on cutoff). A pitch amount of 3% is
  vibrato; 100% is an octave.
- **Audio-rate and control patches differ in feel, not in kind.** Noise, VCO and VCF are signals at
  audio rate; `MATRIX_SOURCES[i].signal` tells them apart if you want a different colour. There is no
  separate "audio patch" parameter.
- **Big targets beat precise sockets.** The matrix needs no cables: clicking a cell, or a source and
  then a destination, creates a patch; dragging sideways on a cell sets the amount. Highlight every
  valid destination for a selected source; with 6 destinations there is never a long list.
- **Notes live as long as ENV 1.** If your UI lets a user take ENV 1 off the amp, say that the note
  still ends with ENV 1.
- **Cells can be automated.** Scheduled parameter events and the sequencer's per-step changes reach
  them, so a patch can change per step. The generic LFO layer acts on the voice controls, not on cells.

## Upgrade checklist

- Rebuild `lib/` and the WASM with the engine (the instrument list and `kMaxModParams` changed).
  Do not mix an old worklet with a new engine module.
- `kMaxModParams` rose from 32 to 80 (an instrument or effect may now have up to 80 catalogue rows).
  Nothing changes for existing kinds.
- Nothing existing changes meaning; adopting Matrix is additive.
