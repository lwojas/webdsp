# Matrix synth: guide for clients building a patching UI

The Matrix synth ([matrix-instrument.md](./matrix-instrument.md)) is a semi-modular voice: fixed modules joined
by cables, with the ordinary voice as the default patch. It has no patching UI of its own. Its patch bay is 88
instrument parameters, and a client decides how to show and edit them. This guide covers what the engine and
`webdsp` give you and what a UI needs to decide.

## What you get

1. **A catalogue entry.** `InstrumentKind.Matrix` (id `"matrix"`, category `"synth"`) appears in the instrument
   catalogue from `ready` like every other kind, with all 109 controls and the shared `gain` row listed by
   `runtime.getInstrumentParams(InstrumentKind.Matrix)` (ranges, defaults, units, modulation domain).
2. **Cells as parameters.** Each cell is a normal parameter named `<out>_<in>` (`vcf_cutoff`, `vco_vcfin`), range
   -1..1. The default values are the five cables of the ordinary voice, so a client that never touches a cell has
   a working synth. Set one with `setInstrumentParameter`; automate one like any parameter.
3. **A model.** `import { ... } from "webdsp"`:

| Export | Use |
|---|---|
| `MatrixParam`, `MatrixOut`, `MatrixIn` | Ids of the module knobs, the outs and the ins. |
| `MATRIX_OUTS`, `MATRIX_INS` | Names, stable keys, signal type (audio or control), role (audio, control, output), the module each belongs to, what 100% means. |
| `matrixAmountParam(out, input)` | Parameter id of a cell. `matrixCellOfParam(id)` is the inverse. |
| `matrixPatches(read)` | The cables in use, from any value store. |
| `matrixLoop(patch, others)` | The loop a cable closes (`["vcf","vcf"]`), or null. |
| `matrixWouldFeedback(out, input, patches)` | Colour an empty cell before it is patched. |
| `matrixPatchKind(patch, all)` | `"control"`, `"audio"` or `"feedback"`. |
| `matrixPath(input, patches)` | Where a signal sent into an in ends up (`["vcf","vca","out"]`), or null when nothing connects it to the output. |

```ts
import { InstrumentKind, MatrixIn, MatrixOut, MatrixParam, matrixAmountParam, matrixPatches, matrixPatchKind } from "webdsp";

const synth = runtime.createInstrument(InstrumentKind.Matrix, { bus });
runtime.setInstrumentParameter(synth, MatrixParam.Cutoff, 600);

// LFO 1 -> cutoff, 55%
runtime.setInstrumentParameter(synth, matrixAmountParam(MatrixOut.Lfo1, MatrixIn.Cutoff), 0.55);
// Filter output back into the filter cutoff: feedback
runtime.setInstrumentParameter(synth, matrixAmountParam(MatrixOut.Vcf, MatrixIn.Cutoff), 0.4);
// Take the filter out of the audio path: the oscillator straight into the amp
runtime.setInstrumentParameter(synth, matrixAmountParam(MatrixOut.Vcf, MatrixIn.VcaIn), 0);
runtime.setInstrumentParameter(synth, matrixAmountParam(MatrixOut.Vco, MatrixIn.VcaIn), 1);

const patches = matrixPatches((id) => mine.get(id) ?? defaults.get(id) ?? 0);   // your own value store
patches.map((p) => matrixPatchKind(p, patches));
```

The runtime does not read instrument parameter values back, so keep the values you set, and seed them from the
catalogue defaults, in your own state and feed `matrixPatches` from it.

## Rules of thumb

- **Outs across the top, ins down the side.** That is how a patch bay reads: an out is a signal a module
  produces, an in is a place to send one. Group the outs into audio (VCO, noise, VCF, VCA) and control (LFOs,
  envelopes), and the ins by module (VCO, VCF, VCA, LFOs, output): the order of `MATRIX_INS` already follows the
  audio path.
- **A cable is out, amount, in.** Removing one is writing 0. There is nothing else to store, so a patch fits in a
  project file as the list `matrixPatches` returns (plus the knobs).
- **The default patch is the voice.** Show it as ordinary cells that happen to be filled in, not as hidden
  wiring: a user who sees `VCO > VCF in`, `VCF > VCA in`, `VCA > Out` understands that they can take them out. An
  empty matrix is silent, which is correct.
- **Treat the matrix as one graph, not 88 knobs.** The same list drives every view: a grid, a flow diagram (a
  line per cable), a synth panel (a ring on each knob showing how far cables move it) or chips in a small
  editor. `matrixPatches` is the one piece of state they all read.
- **Show feedback as feedback.** Use `matrixLoop`: it knows that VCF out into its own cutoff is a loop and that
  VCF out into the amp level is not. Colour empty cells with `matrixWouldFeedback`, and explain a loop in plain
  words ("the filter hears its own output"). The engine keeps it bounded, so there is no warning to raise; the
  colour is for understanding, not alarm.
- **Say where a cable goes.** `matrixPath` answers "will I hear this?": an LFO cabled to the cutoff of a filter
  that is not patched to the output goes nowhere.
- **Say what 100% means.** Amounts are normalised, so translate them: `fullScale` in `MATRIX_INS` gives the unit
  (1 octave on pitch, 5 octaves on cutoff, unity gain on an audio in).
- **Audio and control cables differ in feel, not in kind.** `MATRIX_OUTS[i].signal` and `MATRIX_INS[i].role` tell
  them apart if you want a different colour; the engine treats every cable the same way.
- **Big targets beat precise sockets.** A matrix needs no cables to drag: clicking a cell creates a cable (start
  it at half strength, or full for an amp level), and dragging sideways sets the amount.
- **Notes live as long as ENV 1.** If your UI lets a user take ENV 1 off the amp, say that the note still ends
  with ENV 1.
- **Cells can be automated.** Scheduled parameter events reach them, so a patch can change per step. The generic
  LFO layer acts on the module knobs, not on cells.

## Upgrade checklist

- Rebuild `lib/` and the WASM with the engine. Do not mix an old worklet with a new engine module.
- `kMaxModParams` is now 128 (an instrument or effect may have up to 128 catalogue rows).
- If you integrated the first version of Matrix (7 sources by 6 destinations, ids 22..63, `MatrixSource` /
  `MatrixDest`, a `noise` knob), the layout changed: ids, enums, names and the noise control are different, and the
  audio path is now patchable. Nothing else changes meaning.
