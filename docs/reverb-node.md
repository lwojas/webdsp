# Native reverb node

ECS-15. Implements the Dattorro plate reverb candidate recommended by
`docs/effects-algorithm-survey.md` (ECS-14) as a native `DSPNode`, records the measurements
`docs/dsp-audit.md` (ECS-13) flagged as needed before committing to it, and documents the
listening evaluation. As of commit range starting at `1cc6e45`.

## What was built

- `native/src/dsp/reverb.h` — `Reverb`, a mono-in/stereo-out `DSPNode` re-derived directly
  from Jon Dattorro, ["Effect Design, Part 1: Reverberator and Other
  Filters"](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf) (JAES 45(9), 1997),
  Fig. 1 / Table 1 / Table 2 — not ported from any third-party implementation, per this
  project's "own a small algorithm" bias (`ARCHITECTURE.md`, "DSP library investigation").
  All delay-line lengths, lattice coefficients, and the stereo output tap structure are the
  paper's own published defaults, linearly scaled from its reference sample rate (29761 Hz)
  to the engine's actual sample rate — the standard way this specific algorithm is ported to
  44.1/48 kHz.
- Three new `NodeParam` ids, added identically to `native/src/params.h` and
  `src/runtime/types.ts`, pinned by `test/paramIds.test.ts`:
  - `ReverbDecay` (7) — tank per-loop feedback gain, 0..1 clamped to 0.9999, controls tail
    length.
  - `ReverbDamping` (8) — tank high-frequency damping, 0..1.
  - `ReverbMix` (9) — dry/wet, 0..1. Defaults to 0 (bypass), matching `Delay`/`BiquadFilter`'s
    "inert until parameterized" convention — a freshly-configured bus's reverb costs one
    cheap early-out per block until a caller sets it.
- `Bus`'s `DSPChain` capacity bumped from 2 to 3 (`native/src/bus.h`); the chain is now
  filter → delay → reverb, same order for the master bus and every track bus.
- `src/worklet/generated/engine.js` / `lib/` rebuilt (`npm run build:wasm`, `npm run
  build:lib`) to carry the new node through to the browser build and the published package.

One deliberate simplification vs. a literal reading of the paper's Fig. 1: the block diagram
shows the "decay" gain applied at two points per tank loop (gain staging for the 24-bit
fixed-point hardware the paper targets). This implementation applies it once, at the point
where each tank's recirculating output is summed into the *other* tank's input — the single
coefficient that actually sets the per-loop feedback gain (and therefore RT60), functionally
equivalent for a float32 implementation with no fixed-point headroom concern. See the comment
at the top of `reverb.h` for the full reasoning.

Input diffusion coefficients (0.75 / 0.625), decay-diffusion-1 coefficient (0.70),
decay-diffusion-2 coefficient (`clamp(decay + 0.15, 0.25, 0.50)`), and input bandwidth
(0.9995) are the paper's own Table 1 defaults, fixed rather than exposed as params — matching
Delay's own precedent of fixing decorrelation/tuning constants and exposing only the
musically load-bearing controls (three, same count as Delay's Time/Feedback/Mix).

## Testing

| Layer | Coverage | Result |
|---|---|---|
| `native/test/reverb_test.cpp` (new) | Bypass-by-default is an exact passthrough; silence in → silence out; `reset()` clears accumulated tank state; out-of-range params (mix=5, decay=100, damping=-3) stay clamped and the output stays finite and bounded; an impulse produces a tail that builds over the diffusion chain's latency then decays monotonically (not sustained/growing); identical params + fresh instances produce bit-identical output; a mono impulse produces genuinely different L/R output (Table 2's tap structure is live); mono channel count (numChannels=1) doesn't crash and stays finite | **26644/26644 assertions pass** |
| `native/test/smoke.mjs` (extended) | Same `DSPNode`, exercised through the actual compiled WASM ABI (`engine.node.mjs`): an audible decaying tail persists on the master bus after the dry source voice stops; the tail actually decays; a track bus's own reverb reaches the master output once summed in; the filter → delay → reverb three-node chain doesn't disturb the filter/delay behavior already covered by this file's earlier tests | pass |
| `test/paramIds.test.ts` (extended) | `NodeParam.ReverbDecay/Damping/Mix` = 7/8/9 pinned against `native/src/params.h` | pass |

"Native/WASM consistency": the native test and the smoke test exercise the identical
`reverb.h` source, compiled by two different toolchains (`clang++` natively, `emcc` to WASM)
into two different artifacts — both passing on the same scenarios (bypass, decay shape,
finite output) is the consistency check available here, the same pattern
`native/test/resampler_test.cpp` / `native/test/smoke.mjs` already establish for the rest of
the engine.

Run: `npm run test:native && npm run test` (or `npm run test:all`).

## Listening evaluation

Rendered directly from the compiled engine (not a mockup) and published for audition,
alongside the measured decay curve below:
**https://claude.ai/artifact/D6e8HadafVN6Ge9hwUfiFy**

Objective supporting evidence (50 ms-window RMS envelope of the impulse render, left
channel): builds over ~100 ms as the four-stage input diffuser fills, then decays smoothly
and monotonically for the full 5 s render — no periodic spikes (which would indicate
comb/metallic ringing), no sustain, no runaway. -20 dB by ~0.6 s, -40 dB by ~2.3 s at
decay=0.6/damping=0.25. This is evidence the algorithm behaves correctly, not a substitute
for the subjective judgment call in the artifact above.

## Measured costs

**WASM binary size** (`src/worklet/generated/engine.js`, browser build): 56559 → 65321 bytes,
**+8762 bytes (+15.5%)**.

**Memory**: one `Reverb` instance allocates ~36300 float32 samples across its delay lines and
lattices at 48 kHz (~142 KB). `Bus` owns one unconditionally (master + all `kMaxTrackBuses` =
32 track buses are pre-allocated at `Engine::init()`, same as the existing filter/delay) —
**~4.6 MB total**, paid regardless of whether any bus's reverb is ever turned on. This is the
memory cost `docs/dsp-audit.md` §4's measurement question flagged as needing confirmation
before committing to Dattorro over Freeverb; it's a fixed, known, one-time allocation, not a
per-voice or per-block cost.

**CPU** (`engine.node.mjs`, Node — no browser JIT warm-up modeled; % of one 128-frame render
quantum's real-time budget at 48 kHz, timed over 4000 blocks):

| Scenario | Reverb off | Reverb on |
|---|---|---|
| Master bus, 1 voice | 0.29% | 0.56% |
| 8 track buses, 8 voices, all reverbing | 0.51% | 3.19% |
| 32 track buses, 32 voices, all reverbing | 1.37% | 11.19% |

Worst case (every one of 32 track buses plus master actively reverbing at once) costs about
11% of one render thread — comfortably within budget with substantial headroom left for
everything else the engine does per block.

## What's not done here

- **Application-layer exposure.** Per `ARCHITECTURE.md`'s "Adding a new master-bus module"
  distinction: the engine change is complete and the capability is fully reachable through
  the public API (`AudioRuntime.setNodeParameter(bus, NodeParam.ReverbMix, ...)` etc.) with
  zero further engine work, but no UI exists in this repo's demo app (`src/app/`) or in
  `webseq` yet — tracked as ECS-21 (`webseq` repo), same shape as Delay's existing
  UI gap noted in `docs/dsp-audit.md` §4/§7.
- **Pre-delay and the four fixed decorrelation coefficients are not exposed as params.** They
  match the paper's own defaults and are fine as fixed constants for v1; exposing more of
  them (in particular pre-delay, useful for placing a reverb "behind" a mix) is a natural
  follow-up if the application layer wants it, not required by this ticket's "useful
  controls" bar.
- **Freeverb fallback was not needed.** The survey flagged Freeverb as the pressure-release
  valve if Dattorro's implementation cost proved too high; it didn't (see above), so it
  wasn't built.
