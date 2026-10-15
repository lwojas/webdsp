# Synth DSP reference (research breadcrumbs for new instruments)

A small, practical menu of DSP techniques for building the next webdsp **instrument machine**
(see [`../instruments.md`](../instruments.md) and the precedent, [`../kick-instrument.md`](../kick-instrument.md)).
It is a starting point for choosing a direction, not a textbook and not a framework.

| File | Use it for |
|---|---|
| [`techniques.md`](./techniques.md) | 28 techniques: character, signal shape, cost, webdsp reuse, controls, source |
| [`instruments.md`](./instruments.md) | Instrument concepts + ranked shortlist ("where do I look first?") |

## How to use it

1. Read the *Constraints* below (1 minute).
2. Find your sound in [`instruments.md`](./instruments.md) or scan `techniques.md` headings.
3. Pick **one** technique; read its entry and open its source only if the sketch is not enough.
4. Implement as an `Instrument` following "Adding a new instrument" in `../instruments.md`.
   Expose only musical controls; the technique stays private.
5. Stop researching. Tune by ear (the Kick WAV-dump workflow in `kick_test.cpp` applies).

## Constraints that shape every choice (from the actual engine)

- One instance = one **monophonic** machine; overlap = more instances (max 64 instances).
- Runs in WASM inside an AudioWorklet, 128-frame blocks. `render()` must **not allocate**;
  all buffers (delay lines, mode banks) are sized in `configure()`/constructor.
- Output is *accumulated* into a bus; track FX, pan, master, capture come for free.
- Deterministic: reset phases/noise seeds on a fresh note (existing tests assume it).
- Retrigger must be continuous (carry phase/envelope level) to avoid clicks.
- Existing DSP to reuse first (`native/src/dsp/`): `AdsrEnvelope`, `BiquadFilter`, `Saturation`
  (tanh + asymmetry + DC blocker), `Bitcrusher`, `Phaser`, `Delay`, `Reverb`, `Panner`.
  Not present: oscillators beyond Kick's sine, exponential envelope helper (Kick has a one-pole
  decay), oversampling, anti-aliased waveshaping, modal/resonator bank, comb/delay-line string.
- No oversampling infrastructure: aliasing mitigation is a per-instrument decision.

## Is a technique worth implementing?

Yes if it: (a) sounds clearly different from osc + env + filter, (b) fits in roughly
< 40 flops/sample/instance (most entries here do, ~64 instances at 48 kHz is ample),
(c) can be driven by ≤ 6 musical controls, (d) is deterministic and bounded.
No if it needs offline analysis data, a PDE grid, FFT blocks bigger than a render block,
or a circuit solver.

## Conventions and honesty

Technique descriptions summarise the cited literature. Sections labelled **Recommendation**
are this document's opinion, not established fact. Cost ratings are qualitative estimates,
not benchmarks. Source links were located via search; open the paper to confirm detail before
relying on exact numbers.

## Deliberately not covered

Full PDE/finite-difference physical models, wave-digital circuit solvers, GPU modal banks,
neural/ML synthesis, sample-based/granular engines, general FFT processing, modulation
matrices or a universal synth framework.
