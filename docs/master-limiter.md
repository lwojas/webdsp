# Master limiter and metering

## Signal path

```
voices -> track bus chain -> gain/pan -> [sum / bus-to-bus routing]
       -> master bus chain -> master gain/pan -> (captures tap here) -> MASTER LIMITER -> output
```

The limiter (`native/src/dsp/limiter.h`, driven from `Engine::process`) is the last stage. It is
not an FX-chain effect and exists once, on the master output only; track buses are never
limited. `NodeParam.Limiter*` sent to a track bus is ignored.

**Behaviour.** Zero latency: per sample, gain is set so `|x| * gain <= ceiling` (instant
attack), then recovers to unity with a one-pole release. A signal at or under the ceiling gets
gain exactly 1 and passes bit-identical. No look-ahead means no added latency, so scheduling
and captures stay sample-accurate; the trade-off is that a hard transient is turned down in the
sample it occurs rather than ahead of time. It is a safety limiter, not a mastering tool.
Stereo-linked (peak of both channels), allocation-free, deterministic.

| Param (MASTER_BUS) | Range | Default |
| --- | --- | --- |
| `NodeParam.LimiterEnable` | 0 / 1 | 1 (on) |
| `NodeParam.LimiterCeiling` | -24..0 dBFS | -0.3 |
| `NodeParam.LimiterRelease` | 5..2000 ms | 100 |

`AudioRuntime.setMasterLimiter({ enabled, ceilingDb, releaseMs })` wraps these. Captures
(`startCapture` / `armCapture`) tap the master mix *before* the limiter.

## Metering

The engine measures peaks as part of `process()` (one abs/max pass per fed bus plus master;
dormant buses cost nothing) and accumulates them. `readMeters()` (`webdsp_read_meters`) returns
and clears the accumulator, so a reader at any rate sees the true maximum since its previous
read and cannot miss a peak. Layout (`Engine::kMeter*`, linear, 1.0 = 0 dBFS):

| Index | Value |
| --- | --- |
| 0 | master mix peak **before** the limiter |
| 1 | master output peak after the limiter |
| 2 | deepest limiter gain reduction in the interval, dB (<= 0) |
| 3 + i | peak of track bus `i + 1` after its chain, gain and pan |

**Clipping** is `>= 1.0` (`CLIP_THRESHOLD`): for a track, that track bus clipped; for master,
the mix reached the limiter at or above 0 dBFS (the output itself never exceeds the ceiling
while the limiter is on).

## Boundary

The worklet reads the array every 8 blocks (~47 Hz) and posts one `meters` event
(`Float32Array`, 67 floats). Silence sends nothing (one trailing all-zero event so the UI sees
the signal fall). `AudioRuntime.onMeters(fn)` / `getMeters()` deliver a `MeterSnapshot`
(`masterIn`, `masterOut`, `gainReductionDb`, `buses[busId - 1]`); clients never see samples.

Display logic shared by clients lives in `src/runtime/meters.ts`: `METER_LED_DB` /
`metersLitLeds()` (10 LED thresholds, top 3 red, the last only at 0 dBFS) and `MeterTracker`
(master level falls at 24 dB/s; clip flags stay lit 1.5 s after the last clipping peak, then
clear — no permanent peak-hold). Time is passed in, so it is unit-testable.

Tests: `native/test/limiter_meter_test.cpp`, `test/meters.test.ts`, ABI section of
`native/test/smoke.mjs`.
