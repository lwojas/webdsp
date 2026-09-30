# Native saturation node

ECS-17. Implements the saturation candidate recommended by `docs/effects-algorithm-survey.md`
(ECS-14) as a native `DSPNode`: a single memoryless `tanh` waveshaper with drive, asymmetry,
output gain, and dry/wet mix — deliberately *not* a general-purpose distortion framework
(multiple selectable curves, per-curve parameter sets, a built-in oversampling engine), per
the ticket's own instruction to avoid one.

## What was built

- `native/src/dsp/saturation.h` — `Saturation`, a stateless-per-sample `DSPNode`: dry sample
  → pre-gain (`SatDrive`, dB) → `tanh(x·drive + asymmetry·bias)` → a one-pole DC blocker →
  output gain (`SatOutputGain`, dB) → dry/wet mix (`SatMix`). Asymmetry biases the shaper's
  *input* rather than switching between two curves for the positive/negative halves — the
  standard cheap way to get even-harmonic ("tube-like") content out of a single
  odd-symmetric `tanh`, per the DAFX nonlinear-processing chapter the survey cites.
- Four new `NodeParam` ids, added identically to `native/src/params.h` and
  `src/runtime/types.ts`, pinned by `test/paramIds.test.ts`:
  - `SatDrive` (16, dB, 0..40) — pre-gain into the shaper
  - `SatAsymmetry` (17, -1..1, 0 = symmetric) — biases the curve for even-harmonic content
  - `SatOutputGain` (18, dB, -24..24) — post-shaper trim/makeup
  - `SatMix` (19, 0..1) — 0 = bypass, matching Delay/Reverb/Filter's "inert until
    parameterized" convention (not Compressor's separate `bypassed_` flag — saturation has
    one obvious "off" dial, a dry/wet mix, the same way delay/reverb do)
- `Bus`'s `DSPChain` capacity bumped from 4 to 5 (`native/src/bus.h`); the chain is now
  filter → delay → reverb → compressor → **saturator**, for the master bus and every track
  bus. The saturator runs last, after the compressor: dynamics are shaped first, then the
  saturator adds harmonic "glue"/warmth to the fully-processed signal — the conventional
  position for a final-stage saturator/exciter in a mastering-style chain.
- `src/worklet/generated/engine.js` / `lib/` rebuilt to carry the node through to the browser
  build and the published package.

### DC handling — a correctness bug caught and fixed while building this

Asymmetry biases `tanh`'s input, which necessarily introduces a DC offset into the shaped
signal (an asymmetric curve fed a zero-mean input doesn't itself produce zero-mean output).
The first version removed it with the textbook one-pole DC blocker
(`y[n] = x[n] - x[n-1] + R·y[n-1]`, `R = 0.995`) and nothing else. Measuring the node's
*bounds* (per the ticket's done criteria) with a heavily-driven, low-frequency,
near-square-wave input — the pathological case for this filter topology, not a hypothetical
one — found the unclamped blocker ringing up to **~1.9× the input's peak** before settling: a
real transient overshoot from the filter's own impulse response (its L1 norm is close to 2,
not the ~1.0025 the steady-state sinusoidal frequency response alone would suggest), confirmed
numerically before touching the fix (reducing `R` made the overshoot *worse*, up to ~1.94×,
while also raising the corner frequency into audible bass territory — no `R` choice both
fixes this and keeps a true "DC blocker" corner). The fix: clamp the blocker's own recursive
state to `[-1, 1]` every sample (the same nominal range `tanh` itself is bounded to) — a
standard anti-windup-style guard for a leaky integrator/differentiator, cheaper than adding a
second limiter stage, and confirmed by direct measurement to bring worst-case overshoot to
exactly `1.0` across the same test sweep. `native/test/saturation_test.cpp`'s
`testOutputStaysWithinUnityRegardlessOfDrive` pins this. A simpler alternative considered and
rejected: statically subtracting `tanh(bias)` (the shaper's response to silence) instead of
running a dynamic filter — this correctly zeroes silence-in/silence-out at any asymmetry
(no state needed for that case) but was measured to *not* track a real driven signal's actual
DC bias (in one measured case it made the mean *worse*, from +0.11 to -0.79, because the
signal's true average under heavy drive is nothing like `tanh(bias)`) — kept as the reason a
stateful blocker is warranted here rather than a memoryless correction, despite the survey's
"no extra state" framing for a first cut.

## Testing

| Layer | Coverage | Result |
|---|---|---|
| `native/test/saturation_test.cpp` (new) | Bypass-by-default (`mix=0`) is an exact passthrough; silence in → silence out at zero asymmetry; `reset()` clears DC-blocker state but not mix/drive/asymmetry; out-of-range params (drive=-1000, asymmetry=999, output gain=1000, mix=5) stay clamped and output stays finite and bounded; output never exceeds unity regardless of drive (the DC-blocker overshoot fix, see above); heavier drive measurably raises RMS (a sine pushed toward a near-square wave, unlike compression which lowers RMS); output gain scales level by the expected dB ratio; mix blends strictly between dry and fully-wet; asymmetry=0 leaves the settled mean at ~0; asymmetry=1 pulls a heavily-driven signal's settled mean back toward 0 (not exactly, see above) instead of leaving it near `tanh(1.5)≈0.905`; asymmetry vs. symmetric curves produce audibly different sample-by-sample output; a silent channel stays silent independent of a simultaneously-driven channel; identical params + fresh instances produce bit-identical output | **19481/19481 assertions pass** |
| `native/test/smoke.mjs` (extended) | Same `DSPNode`, exercised through the compiled WASM ABI: heavy drive measurably raises RMS on the master bus; asymmetry stays bounded (<2.0) alongside drive; output gain measurably raises the level further while staying finite; a track bus's own saturator reaches the master output once summed in; the five-node chain doesn't disturb filter/delay/reverb/compressor behavior already covered by this file's earlier tests | pass |
| `test/paramIds.test.ts` (extended) | `NodeParam.SatDrive/SatAsymmetry/SatOutputGain/SatMix` = 16-19 pinned against `native/src/params.h` | pass |

"Native/WASM consistency": as with reverb/compressor, the native test and the smoke test
exercise the identical `saturation.h` source through two different toolchains (`clang++`
natively, `emcc` to WASM) — the same mechanism that caught the compressor's topology bug
previously caught this node's DC-blocker overshoot, this time via the native unit test's own
explicit bounds check rather than the smoke test.

Run: `npm run test:native && npm run test` (or `npm run test:all`).

## Listening evaluation

Rendered directly from the compiled engine (not a mockup): a three-note chord (A3/C4/E4)
alternating quiet (~-24 dBFS) and loud (~-1 dBFS) passages, dry vs. saturated at three drive
levels (the last with asymmetry engaged too), published alongside the measured RMS table and
the aliasing assessment below:
**https://claude.ai/artifact/N9xJTQyEe2orWYCEAUdBxc**

Objective supporting evidence (RMS, quiet vs. loud passage): the loud/quiet RMS ratio narrows
from 5.7× dry toward 1.2× at the heaviest setting — drive audibly (and measurably) compresses
the *gap* between passages by saturating the loud section's peaks, even though this node has
no dynamics section of its own. This is a real, expected side effect of any hard-clipping
waveshaper, not a bug, and is worth understanding before pairing saturation with (or in place
of) a compressor on the same bus.

### Aliasing assessment (per the ticket, measured before deciding against oversampling)

A 7kHz tone was driven at increasing amounts and read back with a Goertzel filter at the true
3rd harmonic (21kHz — an expected product of `tanh`'s odd-symmetric curve) and at 13kHz — a
frequency with no harmonic relationship to 7kHz, only reachable by the 5th harmonic (35kHz)
folding back past the 24kHz Nyquist limit (48 − 35 = 13kHz), i.e. unambiguous evidence of
aliasing rather than a coincidental true harmonic:

| Drive | 3rd harmonic, 21kHz (expected) | Aliased energy, 13kHz (unexpected) |
|---|---|---|
| 0 dB | 0.032 | 0.0018 |
| 12 dB | 0.297 | 0.107 |
| 24 dB | 0.413 | 0.237 |
| 40 dB | 0.417 | 0.244 |

Aliased energy is real, grows quickly with drive, and plateaus around ~24% of the fundamental
at heavy drive (12dB is already ~9%). **Decision: no oversampling in v1**, per
`docs/effects-algorithm-survey.md`'s own recommendation — this is the documented, accepted
tradeoff for a single memoryless waveshaper, not an oversight: the chord clips above stay
musically useful through moderate drive, and the aliasing is most exposed by content this node
isn't particularly targeted at (sustained high-frequency tones at maximum drive), not typical
program material. Oversampling (2×/4×, or antiderivative antialiasing per Parker/
Zavalishin/Bilbao, DAFx 2016 — both cited in the survey) remains the documented upgrade path if
a future use case needs it.

## Measured costs

**WASM binary size** (`src/worklet/generated/engine.js`, browser build): 73554 → 75626 bytes,
**+2072 bytes (+2.8%)**.

**Memory**: a handful of scalar floats per instance (drive/asymmetry/output-gain/mix plus a
2-channel DC-blocker state pair) — under 1 KB total across the master bus + all 32 track
buses, effectively free, same order of magnitude as the compressor.

**CPU** (`engine.node.mjs`, Node — % of one 128-frame render quantum's real-time budget at
48 kHz, timed over 4000 blocks):

| Scenario | Saturation off | Saturation on |
|---|---|---|
| Master bus, 1 voice | 0.29% | 0.39% |
| 8 track buses, 8 voices, all saturating | 0.52% | 1.54% |
| 32 track buses, 32 voices, all saturating | 1.36% | 5.41% |

Somewhat costlier than the compressor (which has no per-sample transcendental call) since
`tanh` is computed once per sample per channel; still cheap in absolute terms and well under
reverb's cost (11.2% worst case).

## What's not done here

- **Application-layer exposure.** Same shape as delay/reverb/compressor: the engine change is
  complete and fully reachable through the public API
  (`AudioRuntime.setNodeParameter(bus, NodeParam.SatDrive, ...)` etc.) with zero further
  engine work, but no UI exists yet in this repo's demo app or in `webseq`.
- **Oversampling.** Explicitly deferred per the aliasing assessment above; the documented
  upgrade path if a future use case needs it, not a rework of this node's shape.
- **Alternative curves.** The survey's "smallest useful" recommendation was a single fixed
  curve (`tanh` or cubic soft-clip); `tanh` was chosen and no second curve/mode was added,
  consistent with the ticket's explicit instruction to avoid a generalized distortion
  framework.
