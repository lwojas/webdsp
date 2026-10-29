# Glitch FX

`EffectKind.Glitch` (kind 11): a ring-buffer performance effect for a bus. One node, three behaviours chosen by
`GlitchMode`. Source: `native/src/dsp/glitch.h`; tests: `native/test/glitch_test.cpp`, `native/test/smoke.mjs`.

The buffer always records the bus (4 s, one ring per channel, shared positions so the stereo image is kept), so a
repeat or a tape read has history the instant it is engaged. `mix` 0 is an exact bypass; the 0 → >0 edge **engages**
the effect, and the mode and division in force at that moment are used.

| Param (`NodeParam`) | Range | Meaning |
|---|---|---|
| `GlitchMix` (77) | 0..1 | Dry/wet (Repeat, Tape) or depth (Gate). Smoothed over ~8 ms: engaging and releasing never click. |
| `GlitchMode` (78) | step 0..2 | `GlitchMode.Repeat`, `Tape`, `Gate`. Read at engage. |
| `GlitchDivision` (79) | step 0..5 | 1/2, 1/4, 1/8, 1/16, 1/32, 1/64 of the transport beat grid (follows BPM). |
| `GlitchSpeed` (80) | -1..2 | Read rate of the loop or the tape: 1 untouched, 0 stopped, negative backwards. LFO-modulatable. |

- **Repeat**: captures the last `division` when engaged and loops it, recording frozen so the capture is not
  overwritten. A division change while engaged takes effect at the loop's next wrap and keeps the *newest* slice, so
  stepping 1/4 → 1/8 → 1/16 → 1/32 is an accelerating roll. Seams have ~1.5 ms fades. `speed` reads the loop slower,
  faster or backwards.
- **Tape**: reads the live signal back through the buffer at `speed` (capped at 1: it cannot read the future). The read
  position starts at the write position, so engaging is seamless; slowing lets the delay grow like a tape machine
  losing speed, and the output fades out as speed approaches 0 (silence, not DC). Reverse plays out of up to ~4 s of history, then goes silent.
- **Gate**: a `division`-long square, open for the first half, closed for the second, 1.5 ms slews. Restarts open at engage.

Tempo comes from the chain's clock like Dub Delay (`setTempo`). A loop or gate longer than the buffer at a slow tempo
is halved until it fits.

Typical performance use (the sweep level of a held pad drives these): Repeat with `division` stepped 1 → 4 over a
bar; Tape with `speed` ramped 1 → 0 (stop) or → 0.5 (half speed) or → -1 (reverse) with a scheduled parameter ramp;
Gate with `mix` as depth.
