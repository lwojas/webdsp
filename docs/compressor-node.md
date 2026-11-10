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
  levels, so a bus compressor doesn't shift the stereo image. No sidechain input in the original
  ticket; it was added later (see "Sidechain").
- Six new `NodeParam` ids, added identically to `native/src/params.h` and
  `src/runtime/types.ts`, pinned by `test/paramIds.test.ts`:
  - `CompThreshold` (10, dB, -60..0)
  - `CompRatio` (11, 1..20) — 1 is a mathematically exact no-op regardless of every other
    param, and is also the construction default
  - `CompAttack` (12, seconds, 0.0001..1)
  - `CompRelease` (13, seconds, 0.001..3)
  - `CompKnee` (14, dB, 0..24)
  - `CompMakeup` (15, dB, -24..24)
  - `CompSidechain` (81, bus id 0..64) — see "Sidechain" below
  - Bypassed by default (an explicit `bypassed_` flag, matching `BiquadFilter`'s convention
    rather than Delay/Reverb's `mix=0` convention — a compressor has no single "mix" dial
    that's obviously "off"): any `setParam` call un-bypasses it.
- `Bus`'s `DSPChain` capacity bumped from 3 to 4 (`native/src/bus.h`); the chain is now
  filter → delay → reverb → **compressor**, for the master bus and every track bus. The
  compressor runs last — the conventional position for bus/glue compression, shaping the
  dynamics of the fully-processed signal rather than just the dry input.
- `src/worklet/generated/engine.js` / `lib/` rebuilt to carry the node through to the browser
  build and the published package.

**## Detector and ballistics (revised for drum-bus use)

The first design smoothed the *level in dB* (a one-pole attack/release on `20*log10(max|x|)` of the raw rectified
signal) and only then applied the gain curve. Measured against analytic signals and a kick/snare/hat/perc pattern
(`native/test/compressor_drumbus_test.cpp`), that had real problems:

| Finding | Evidence | Cause | Change |
|---|---|---|---|
| The detector under-read the signal, so **Ratio and Threshold did not mean what the display says**. | 1 kHz sine 18 dB over threshold: 2:1 gave -7.9 dB (maths: -9.0), 10:1 gave -14.2 (maths: -16.2), 4:1 -> 20:1 differed by only 3 dB. On the drum pattern 8:1 vs 20:1 differed by 0.8 dB. | The dB-domain envelope is a *log-mean* of a rectified wave: every zero crossing pulls it far below the peak, and a 3-10 ms transient never lifts it to its real level. | Linear-domain **peak follower** (instant attack, fixed 20 ms decay) feeds the curve, so Threshold is "peaks above this are reduced" and steady reduction equals `over * (1 - 1/R)` (now within 0.4 dB). |
| **Attack barely worked** on drums: with 0.5-100 ms attack the first snare transient changed by 0 dB (the envelope never got there at all). | Snare-hit peak change vs attack: 0 dB for everything >= 3 ms. | Same: the slow smoothing sat on the level, where a dB-domain one-pole is not a gain ballistic. | Attack/release now smooth the **gain reduction** (attack while it deepens, release while it recovers). Slower attack now passes measurably more of the hit (-7.0 dB at 0.5 ms, -0.5 dB at 100 ms). |
| Low-frequency gain ripple. | 50 Hz at 3 ms attack / 50 ms release: 0.8 dB peak-to-peak. | Rectified LF wave dips to zero twice per cycle. | The 20 ms peak hold bridges the zero crossings: 0.16 dB, and 40-50 Hz gets the same gain as 1 kHz. |
| Not a bug, but the cause of "the bass disappears". | Threshold -40 on a kit peaking at -3 dBFS = about 25 dB of reduction on the kick, with makeup 0. | A full-band peak detector keys on the kick (the loudest thing in the kit), and there is no automatic makeup, so everything drops by the kick's reduction. The old under-read hid this until the threshold was far down, which is why it felt like a cliff. | No hidden fix: no automatic makeup is added (makeup stays explicit). The UI now shows threshold vs peak, reduction and the explanation; presets pick thresholds that land on 3-12 dB of reduction and supply makeup. |

Verified unchanged and correct: the static soft-knee curve (the tutorial's), stereo linking (max of the channels), the
makeup stage (added after the reduction, so it never changes it: the meter reads the same at +0 and +6 dB makeup),
parameter ranges, and finite/bounded behaviour. Ratio 1 is still an exact no-op.

Calibration point: on the (hot, +3.7 dBFS) probe kit at threshold -18, 4:1, 10 ms / 100 ms, the deepest 20 ms window
went from -8.1 dB (old) to -12.8 dB (new) for the same settings; presets therefore sit about 11-16 dB below the kit's peak,
not the 6 dB the static curve alone suggests, because the attack lets the start of each hit through.

## Metering and UI

`Compressor::takeMeter()` reports the input peak, deepest gain reduction (dB, before makeup) and output peak since the
previous call. The engine reads it once per block per bus and appends it to the meter array: after the existing
`3 + 64` entries come three floats per bus (master first, then track bus 1..64), `Engine::kMeterDynamics`. A chain
with several compressors reports the largest peaks and the deepest reduction. `compressorMeter(snapshot, bus)` decodes it
in TypeScript (zeros on an older engine build, and for a bus without a compressor). The meter count is now 262.

`src/runtime/compressor.ts` holds the curve (mirrors the DSP), the presets, `LoudnessChange` (for the level-matched
A/B) and a gain-reduction history. The dev app's **Compressor** panel (`src/app/components/CompressorPanel.tsx`) draws:
the transfer curve (1:1 diagonal, threshold, knee, ratio slope, shaded reduction, live dot), input/GR/output bars with a
threshold tick, a 3 s GR trace and a one-sentence explanation of threshold -> ratio -> reduction for the current signal.

### Presets (drum bus)

| Preset | Ratio | Attack | Release | Knee | Peak GR | Makeup |
|---|---|---|---|---|---|---|
| Drum Glue | 2:1 | 25 ms | 100 ms | 8 dB | ~3 dB | +1.3 dB |
| Punch | 3:1 | 28 ms | 90 ms | 6 dB | ~4 dB | +1.7 dB |
| Tight | 4:1 | 9 ms | 70 ms | 6 dB | ~5.5 dB | +2.3 dB |
| Smash | 10:1 | 4 ms | 50 ms | 2 dB | ~12 dB | +5.5 dB |

The threshold is *relative*: the panel places it a calibrated distance below the loudest hits it measures at the
compressor input (or below -3 dBFS if nothing is playing), so a preset lands on its reduction at any kit level. Makeup
is an explicit value (about 0.42 x the peak reduction) that measured as equal RMS on the test pattern; loading a
preset never adds hidden gain. The A/B button keeps the compressor running at 1:1 with the tracked loudness change
applied as plain gain, so the dry side is not just the quieter or louder one.

## Sidechain

`NodeParam.CompSidechain` (81) names a track bus (`1..64`) whose signal drives the detector; `0` (default) is the compressor's
own input, as before. The gain reduction is still applied to the compressor's own signal, so a bass bus ducks to a kick bus.

```ts
runtime.setEffectParameter(bassBus, comp, NodeParam.CompSidechain, kickBus);
```

- **The key** is the key bus's output after its chain, gain and pan: the tap the sends and meters use. A muted or faded key
  ducks nothing, and a send or main-route level does not matter (cut the key bus's `BusMainGain` to duck from a bus you do not
  want to hear). A key bus that is asleep (never fed) is silence, so the compressor does not reduce.
- **Order.** The engine runs a key bus before the buses that listen to it, whatever their indices: a key is an edge in the
  bus ordering beside the main route and the send, rebuilt whenever a key is set, an effect removed or enabled/disabled. A
  compressor on the master bus can key from any track bus.
- **Refused keys.** A bus keying itself, an unknown bus, or a bus the listener feeds (through routes, sends or other keys:
  a loop) reads as 0, and a later route that would close a loop through a key is refused like any other. `validKey` /
  `leadsTo` in `engine.h`.
- **Not modulatable, not schedulable as a ramp.** It is a stepped selector (`kNoMod`).
- **Hosts.** The key is a bus buffer inside the engine, so a native host (webvst) needs nothing beyond the new parameter id.
  The WASM must be rebuilt (`npm run build:wasm`) for a browser to know the id; an older build ignores it.

Tests: `native/test/compressor_test.cpp` (`testSidechainDetectsOnTheKey`) and `native/test/sidechain_test.cpp` (ducking, bus
index order, a sleeping key, refused loops, master listener, removal).

## Testing

| Layer | Coverage | Result |
|---|---|---|
| `native/test/compressor_drumbus_test.cpp` | Ratio vs analytic reduction (2:1 -9 dB, 10:1 -16.2 dB), threshold monotonic, 40/50 Hz gain equals 1 kHz, attack/release time constants, knee at/below the threshold, makeup independence, drum-pattern checks (threshold, ratio, attack/release), low end kept under heavy compression, preset calibration, meter feed | pass |
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
  NodeParam.CompRatio, ...)` etc.) with zero further engine work, but the dev app has a Compressor panel (see "Metering and UI"); `webseq` has none yet. Follow-up work, same pattern as ECS-21 for reverb.
- **RMS detection mode.** The survey allowed "peak or RMS detector"; peak was chosen as the
  simpler of the two (RMS needs a second smoothing stage — a lowpass on the squared signal —
  on top of the envelope follower already present). A future RMS-detect mode would be an
  additive change, not a rework of this topology.
- **Sidechain filter.** The key is the bus's full-range signal; a high-pass on the detector is not offered.
