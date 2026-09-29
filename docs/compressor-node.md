# Native compressor node

ECS-16. Implements the compressor candidate recommended by `docs/effects-algorithm-survey.md`
(ECS-14) as a native `DSPNode`: a feedforward, log-domain, soft-knee peak compressor, the
"smallest useful" conventional design the survey compared against feedback/RMS variants.

## What was built

- `native/src/dsp/compressor.h` — `Compressor`, a stereo-linked `DSPNode` following
  Giannoulis, Massberg, Reiss, ["Digital Dynamic Range Compressor Design — A Tutorial and
  Analysis"](https://www.researchgate.net/publication/277772168_Digital_Dynamic_Range_Compressor_Design-A_Tutorial_and_Analysis)
  (JAES 60(6), 2012): instantaneous peak level (dB) → one-pole attack/release envelope
  follower (log domain) → static soft-knee gain-computer curve → linear gain (with makeup)
  applied equally to every channel. Stereo-linked via the max of the channels' instantaneous
  levels, so a bus compressor doesn't shift the stereo image. No sidechain input — out of
  scope per the ticket.
- Six new `NodeParam` ids, added identically to `native/src/params.h` and
  `src/runtime/types.ts`, pinned by `test/paramIds.test.ts`:
  - `CompThreshold` (10, dB, -60..0)
  - `CompRatio` (11, 1..20) — 1 is a mathematically exact no-op regardless of every other
    param, and is also the construction default
  - `CompAttack` (12, seconds, 0.0001..1)
  - `CompRelease` (13, seconds, 0.001..3)
  - `CompKnee` (14, dB, 0..24)
  - `CompMakeup` (15, dB, -24..24)
  - Bypassed by default (an explicit `bypassed_` flag, matching `BiquadFilter`'s convention
    rather than Delay/Reverb's `mix=0` convention — a compressor has no single "mix" dial
    that's obviously "off"): any `setParam` call un-bypasses it.
- `Bus`'s `DSPChain` capacity bumped from 3 to 4 (`native/src/bus.h`); the chain is now
  filter → delay → reverb → **compressor**, for the master bus and every track bus. The
  compressor runs last — the conventional position for bus/glue compression, shaping the
  dynamics of the fully-processed signal rather than just the dry input.
- `src/worklet/generated/engine.js` / `lib/` rebuilt to carry the node through to the browser
  build and the published package.

**One implementation pitfall worth recording**: the first version of this node computed the
static gain curve directly from the *raw, unsmoothed* instantaneous peak level, and only
smoothed the resulting gain-reduction value with the attack/release ballistic. This is
subtly wrong for anything but a DC test signal — feeding a rectified sine straight into a
nonlinear curve at audio rate re-derives a new, distorted waveform shape every cycle instead
of tracking the signal's envelope, and it was caught exactly this way: the native unit tests
(driven by settled-window RMS measurements, not raw waveform inspection) all happened to
pass, but the WASM smoke test's track-bus scenario — a real hard number, "RMS should exceed
0.01" — came back at 0.0014, an order of magnitude off from the hand-computed expectation.
Fixed by moving the attack/release smoothing *before* the gain computer (smooth the level,
then compute gain from the smoothed level) — the standard textbook order, and the one the
survey's own reference tutorial presents. Left as a reminder that "the math checks out
against RMS" is not the same test as "the topology matches the paper."

## Testing

| Layer | Coverage | Result |
|---|---|---|
| `native/test/compressor_test.cpp` (new) | Bypass-by-default is an exact passthrough; silence in → silence out; `reset()` clears the envelope but not params/bypass; out-of-range params (threshold=-1000, ratio=999, attack=-5, release=0, knee=-10, makeup=1000) stay clamped and output stays finite and bounded; a signal above threshold is measurably and predictably reduced (within 0.08 of the hand-computed dB target) while a signal below threshold passes through within 5%; makeup gain restores level to within 15% of dry; a sudden loud onset produces no single-sample discontinuity (max jump < 0.5, i.e. click-free); mid-stream parameter changes (as automation would produce) stay finite; identical params + fresh instances produce bit-identical output | **13333/13333 assertions pass** |
| `native/test/smoke.mjs` (extended) | Same `DSPNode`, exercised through the compiled WASM ABI: a loud tone's RMS is measurably reduced once the compressor engages on the master bus; raising makeup gain afterward measurably raises the level back while staying finite; a track bus's own compressor reaches the master output once summed in; the four-node chain doesn't disturb filter/delay/reverb behavior already covered by this file's earlier tests | pass |
| `test/paramIds.test.ts` (extended) | `NodeParam.CompThreshold/Ratio/Attack/Release/Knee/Makeup` = 10-15 pinned against `native/src/params.h` | pass |

"Native/WASM consistency": as with reverb, the native test and the smoke test exercise the
identical `compressor.h` source through two different toolchains (`clang++` natively, `emcc`
to WASM) — this is exactly what caught the topology bug above, since the native tests alone
did not.

Run: `npm run test:native && npm run test` (or `npm run test:all`).

## Listening evaluation

Rendered directly from the compiled engine (not a mockup): a signal alternating quiet
(~-24 dBFS) and loud (~-1 dBFS) bursts, dry vs. compressed (threshold -20dB, ratio 6:1,
attack 5ms, release 150ms, makeup +8dB), published for audition alongside the measured
leveling curve:
**https://claude.ai/artifact/P8H6ALSUk29gGvzZRDWTQh**

Objective supporting evidence (100ms-window RMS, dry vs. compressed): quiet passages go from
~0.044 to ~0.11 (a near-exact +8dB makeup boost, negligible compression — correct, well below
threshold); loud passages go from ~0.636 to ~0.28, matching the hand-computed ~-15.9dB
reduction plus makeup to within a few percent. The brief dip below dry level immediately after
each loud section is the 150ms release catching up — real, audible release ballistics, not an
artifact of an incorrect implementation (confirmed by comparing against the closed-form
steady-state math, not just listening).

## Measured costs

**WASM binary size** (`src/worklet/generated/engine.js`, browser build): 65321 → 73554 bytes,
**+8233 bytes (+12.6%)**.

**Memory**: a handful of scalar floats per instance (no delay lines, unlike reverb/delay) —
under 1 KB total across the master bus + all 32 track buses, effectively free compared to
reverb's ~4.6 MB.

**CPU** (`engine.node.mjs`, Node — % of one 128-frame render quantum's real-time budget at
48 kHz, timed over 4000 blocks):

| Scenario | Compressor off | Compressor on |
|---|---|---|
| Master bus, 1 voice | 0.29% | 0.35% |
| 8 track buses, 8 voices, all compressing | 0.52% | 1.27% |
| 32 track buses, 32 voices, all compressing | 1.37% | 4.10% |

Substantially cheaper than reverb (11.2% worst case) — no delay-line memory to walk, just a
few scalar log/pow operations per sample.

## What's not done here

- **Application-layer exposure.** Same shape as reverb/delay: the engine change is complete
  and fully reachable through the public API (`AudioRuntime.setNodeParameter(bus,
  NodeParam.CompRatio, ...)` etc.) with zero further engine work, but no UI exists yet in this
  repo's demo app or in `webseq`. Follow-up work, same pattern as ECS-21 for reverb.
- **RMS detection mode.** The survey allowed "peak or RMS detector"; peak was chosen as the
  simpler of the two (RMS needs a second smoothing stage — a lowpass on the squared signal —
  on top of the envelope follower already present). A future RMS-detect mode would be an
  additive change, not a rework of this topology.
- **Sidechain input.** Explicitly out of scope per the ticket.
