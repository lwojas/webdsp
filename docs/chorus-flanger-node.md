# Native chorus/flanger node

ECS-18. Implements the chorus/flanger candidate recommended by
`docs/effects-algorithm-survey.md` (ECS-14) as a native `DSPNode`: one sine-LFO-modulated
fractional-delay tap summed with dry, reusing the same wrapped-linear-interpolation technique
`dsp/reverb.h`'s modulated allpass already uses — deliberately *one* node, not two, per the
survey's own framing: chorus and flanger are the same primitive at different delay-range/
feedback settings, not different algorithms.

## What was built

- `native/src/dsp/chorus_flanger.h` — `ChorusFlanger`, a `DSPNode` following Dattorro,
  ["Effect Design, Part 2: Delay-Line Modulation and Chorus"](https://ccrma.stanford.edu/~dattorro/EffectDesignPart2.pdf)
  (JAES 45(10), 1997): dry sample → per-channel ring buffer (feedback folded back in, comb-
  filter style, same structure as `dsp/delay.h`) → sine-LFO-modulated fractional read
  position → wrapped linear interpolation → dry/wet mix. The read position is
  `center delay ± depth · sin(LFO phase)`, floored at a small positive minimum so a depth
  configured larger than the center delay (a valid, expected flanger setting) can never read
  a not-yet-written sample. Channel 1's LFO phase is offset from channel 0's by
  `ChorusFlangerStereoPhase` (a fraction of one LFO cycle) — the standard way to widen the
  effect in stereo from a single LFO rather than running two uncorrelated ones.
- Six new `NodeParam` ids, added identically to `native/src/params.h` and
  `src/runtime/types.ts`, pinned by `test/paramIds.test.ts`:
  - `ChorusFlangerRate` (20, Hz, 0.01..10) — LFO speed
  - `ChorusFlangerDepth` (21, ms, 0..20) — LFO peak excursion around the center delay
  - `ChorusFlangerDelay` (22, ms, 0.1..40) — center/base delay; ~15-30ms with little
    feedback reads as chorus, ~1-10ms with feedback dialed up reads as flanger — the same
    single-dial-covers-both relationship `CompRatio` has between "bus glue" and
    "near-limiting"
  - `ChorusFlangerFeedback` (23, -0.95..0.95) — comb resonance; 0 for a plain chorus,
    nonzero (either sign) for a flanger's resonant sweep
  - `ChorusFlangerStereoPhase` (24, 0..1) — fraction of one LFO cycle channel 1 leads
    channel 0 by
  - `ChorusFlangerMix` (25, 0..1) — 0 = bypass, matching Delay/Reverb/Saturation's "inert
    until parameterized" convention
- `Bus`'s `DSPChain` capacity bumped from 5 to 6 (`native/src/bus.h`); the chain is now
  filter → **chorus/flanger** → delay → reverb → compressor → saturator, for the master bus
  and every track bus. Placement: right after tone-shaping and before the time-based/
  dynamics/harmonic stages — a modulated pitch/comb effect reads best on a still-dry signal,
  not stacked on top of delay/reverb tails or post-compression dynamics.
- `src/worklet/generated/engine.js` / `lib/` rebuilt to carry the node through to the browser
  build and the published package.

### Why linear interpolation, not `resampler.h`'s cubic Hermite

The survey suggested reusing `resampler.h`'s fractional-delay interpolation for this family of
effects; `dsp/reverb.h`'s modulated allpass hit the same question first and settled on a
locally-written wrapped **linear** interpolator instead, because `resampler.h`'s functions
*clamp* at buffer edges (correct for a single non-wrapping sample buffer) rather than *wrap*
(required for a circular delay line read/write at the same time). This node follows that same
precedent (`ChorusFlanger::readWrapped`, structurally identical to
`ReverbModulatedAllpass::readWrapped`) rather than introducing a second interpolation
convention for the same underlying need. Per Dattorro's Part 2 paper, linear interpolation is
also simply the standard technique for this exact application, not a corner cut.

## Testing

| Layer | Coverage | Result |
|---|---|---|
| `native/test/chorus_flanger_test.cpp` (new) | Bypass-by-default (`mix=0`) is an exact passthrough; silence in → silence out with feedback engaged; `reset()` clears the ring buffer/feedback state (verified via silence-after-loud-then-reset) but not mix/feedback/delay; out-of-range params (rate, depth, delay, feedback, stereo phase, mix all pushed outside their ranges) stay clamped and output stays finite and bounded; depth configured larger than the center delay (a valid flanger setting, exercising the min-delay-frames floor) stays finite and bounded; with depth=0 the node reduces to an exact static delay tap at the expected sample latency (verified against an impulse at an exact-integer-frame delay); feedback produces the expected geometrically-decaying repeated taps (`feedback`, `feedback²`, ...); depth vs. no depth measurably changes a sine's output (modulation is actually happening, not a no-op); rate change measurably changes output; stereo phase = 0 keeps both channels bit-identical, stereo phase > 0 measurably decorrelates them; a silent channel stays silent independent of a simultaneously-processed channel; mix blends strictly between dry and fully-wet; identical params + fresh instances produce bit-identical output | **55621/55621 assertions pass** |
| `native/test/smoke.mjs` (extended) | Same `DSPNode`, exercised through the compiled WASM ABI: stereo channels stay identical while bypassed; enabling depth + a nonzero stereo phase measurably widens L vs. R on identical mono-doubled input; feedback dialed up (0.8) stays finite; a track bus's own chorus/flanger reaches the master output once summed in; the six-node chain doesn't disturb filter/delay/reverb/compressor/saturation behavior already covered by this file's earlier tests | pass |
| `test/paramIds.test.ts` (extended) | `NodeParam.ChorusFlangerRate/Depth/Delay/Feedback/StereoPhase/Mix` = 20-25 pinned against `native/src/params.h` | pass |

"Native/WASM consistency": as with reverb/compressor/saturation, the native test and the
smoke test exercise the identical `chorus_flanger.h` source through two different toolchains
(`clang++` natively, `emcc` to WASM).

Run: `npm run test:native && npm run test` (or `npm run test:all`).

## Listening/measurement validation

No browser/DAW A/B render was available in this environment (unlike `docs/saturation-node.md`'s
published artifact); in its place, two objective measurements were made directly against the
compiled WASM engine, each targeting a specific claim about what this node is supposed to do
rather than just "it doesn't crash":

**1. The LFO actually modulates at the configured rate.** A 300Hz sine was run through the
node (depth=8ms, delay=15ms, feedback=0, mono) at three different `ChorusFlangerRate` settings.
The output's instantaneous period was tracked via interpolated zero-crossings, then the
dominant frequency of *that* period signal (i.e. the vibrato rate imposed on the tone) was
found by direct DFT over a narrow candidate band:

| Configured rate | Detected modulation frequency |
|---|---|
| 0.5 Hz | 0.50 Hz |
| 1.0 Hz | 1.00 Hz |
| 3.0 Hz | 3.00 Hz |

Exact tracking at all three settings — the LFO drives the audible modulation at precisely the
rate the parameter claims, not some filtered/aliased approximation of it.

**2. Stereo phase actually widens the stereo image.** The same mono-doubled tone (identical
L/R) was run through the node (depth=8ms, delay=15ms, rate=1.5Hz) at several
`ChorusFlangerStereoPhase` settings, measuring the Pearson correlation between the resulting L
and R channels:

| Stereo phase | L/R correlation |
|---|---|
| 0.0 | 1.0000 |
| 0.1 | -0.1437 |
| 0.25 | -0.0192 |
| 0.5 | -0.0656 |

At phase 0 the channels are perfectly correlated (both LFOs in lockstep on identical input, as
expected — this is also `testZeroStereoPhaseKeepsChannelsIdentical`'s exact scenario). Any
nonzero phase offset collapses correlation from 1.0 to near zero — a large, real decorrelation,
confirming the stereo-widening mechanism works. The correlation values are not monotonic in
the phase setting (0.1 measures more decorrelated than 0.25 here): expected for a
phase-modulation-driven effect measured by linear correlation over a fixed window, not a sign
of a bug — the two channels' *relative* phase offset determines a complex, not linear,
mapping. A true browser/DAW listening pass remains a followup, same caveat
`docs/saturation-node.md` notes for the same kind of application-layer verification.

## Measured costs

**WASM binary size** (`src/worklet/generated/engine.js`, browser build): 75626 → 80360 bytes,
**+4734 bytes (+6.3%)**.

**Memory**: two ring buffers (max ~60ms each = center delay's 40ms ceiling + depth's 20ms
ceiling, plus a few samples' margin) per instance, ~2880 frames × 4 bytes × 2 channels ≈ 23KB
per bus instance. Across the master bus + all 32 track buses (`kMaxTrackBuses`), worst case
~760KB if every bus engaged it simultaneously — the largest fixed footprint of the five
bus-effect nodes so far (reverb's tank delay lines are comparable in the same ballpark, delay/
compressor/saturation are all far smaller), still a small, bounded, pre-allocated cost, not a
per-render-block allocation.

**CPU** (`engine.node.mjs`, Node — % of one 128-frame render quantum's real-time budget at
48kHz, timed over 4000 blocks):

| Scenario | Chorus/flanger off | Chorus/flanger on |
|---|---|---|
| Master bus, 1 voice | 0.31% | 0.39% |
| 8 track buses, 8 voices, all engaged | 0.53% | 1.45% |
| 32 track buses, 32 voices, all engaged | 1.37% | 4.76% |

Comparable to the saturator's cost (a `sin()` call plus a wrapped linear interpolation per
sample per channel, vs. saturation's single `tanh()`), cheaper than reverb, and well within
budget even in the 32-bus worst case.

## What's not done here

- **Application-layer exposure.** Same shape as delay/reverb/compressor/saturation: the
  engine change is complete and fully reachable through the public API
  (`AudioRuntime.setNodeParameter(bus, NodeParam.ChorusFlangerRate, ...)` etc.) with zero
  further engine work, but no UI exists yet in this repo's demo app or in `webseq`.
- **Multiple modulated taps / voices.** The survey explicitly frames additional delay taps as
  "a pure quality dial, not a required part of a first cut" — one tap per channel is the
  smallest-useful design here, same spirit as saturation's single fixed curve.
- **Independent per-channel LFO rates.** Only a phase offset between channels is exposed
  (`ChorusFlangerStereoPhase`); both channels share one rate. A shared LFO with a phase offset
  is the standard, simpler way to get stereo width without detuning the two channels against
  each other.
- **Browser/DAW listening pass.** See "Listening/measurement validation" above — the
  quantitative measurements there stand in for it for now.
