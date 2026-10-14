# Filter slope (12 / 24 dB per octave)

Adds a selectable rolloff slope to `BiquadFilter` (`native/src/dsp/biquad_filter.h`) — the
single filter class used both per voice (`VoiceParam`) and per bus (`NodeParam`), low-pass and
high-pass.

## What the slopes mean

- **12 dB/oct** — two-pole (second-order) response; amplitude falls ~12 dB for every doubling
  of frequency past the cutoff. This is the original filter, unchanged, and the default.
- **24 dB/oct** — four-pole (fourth-order) response; ~24 dB per octave. Steeper, so it removes
  more out-of-band content and sounds "tighter" at the same cutoff.

## Topology

- 12 dB: one RBJ cookbook biquad (transposed direct form II), exactly as before. Output is
  bit-identical to the pre-slope implementation when the slope param is never set.
- 24 dB: two RBJ biquads in series at the **same cutoff**, with the 4th-order Butterworth pole
  Q split **Q₁ = 0.5412, Q₂ = 1.3066** — not the same biquad run twice. Running one
  Q = 0.707 biquad twice gives a Linkwitz-Riley response, which is −6 dB at the cutoff and
  would make the 12→24 switch change the corner level; the Butterworth split keeps the cutoff
  at −3 dB in both modes and the passband maximally flat.

## Resonance

`FilterResonance` keeps its meaning (RBJ Q, 0.707 = flat/Butterworth, default). In 24 dB mode
it scales the high-Q stage only: `Q₂ = 1.3066 · (Q / 0.7071)`, Q₁ stays 0.5412. Consequently:

- Q = 0.707 → exactly maximally-flat 24 dB Butterworth.
- Gain at the cutoff equals `Q` in **both** modes (0.5412 · 1.3066 · Q/0.7071 = Q), so a given
  resonance setting peaks to the same level whichever slope is selected; the 24 dB peak is
  narrower and the skirts steeper.
- Q ≤ 0 is clamped to 0.001 as before; every stage Q is > 0, so both stages are stable.

## Parameter

| Id space | Id | Values |
| --- | --- | --- |
| `VoiceParam.FilterSlope` | 5 | `12` (default) or `24` (dB/oct) |
| `NodeParam.FilterSlope` | 26 | `12` (default) or `24` (dB/oct) |

TypeScript exposes the value space as `FilterSlope { Db12 = 12, Db24 = 24 }`
(`src/runtime/types.ts`). The native side treats any value ≥ 18 as 24 and anything else as
12. Setting only the slope does not engage the filter: it stays bypassed until a cutoff or
mode is set, as before. Existing code that never sets the param is unaffected, and no
existing param id changed.

## State handling

- Each channel keeps separate state for the 12 dB biquad and for the two 24 dB stages; no
  allocation happens in `process`.
- Changing slope on a running filter **crossfades** the two paths over 256 samples (~5 ms at
  48 kHz): the 4th- and 2nd-order responses differ in phase, so a hard swap steps the output
  (measured ~0.12 on a 0.8 sine at 300 Hz). The path being faded in is first set to its DC
  steady state for the current input so it joins without a transient.
- If nothing has been processed yet (fresh or `reset()` filter — e.g. a voice configured
  before its first block) the slope is adopted immediately with no fade.
- `reset()` zeroes all states and cancels any fade; the selected slope persists (like cutoff
  and mode).
- Cutoff is clamped to 20 Hz … 0.49·fs as before, so extreme cutoffs stay stable.

## Tests

`native/test/filter_slope_test.cpp` (run by `npm run test:native`): default equals the
original biquad exactly; measured LP/HP slopes ≈12 and ≈24 dB/oct; −3 dB at cutoff and gain
≈ Q at cutoff in both slopes; click-free slope toggling; finite output across extreme
cutoff/Q; reset behaviour; slope alone doesn't engage the filter; non-standard slope values
snap. `test/paramIds.test.ts` pins the new ids.
