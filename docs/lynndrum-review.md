# LynnDrum vs webdsp: comparative engineering review

Date: 2026-10-25. Read-only study of the local LynnDrum repo (a VST port of the Erica Synths LXR-02 firmware) against
webdsp at `350cf05`. Nothing in either repo was changed by the study. Measurements are native (`clang++ -O2`) probes,
not WASM, and nothing was auditioned. The action plan is [`lynndrum-implementation.md`](./lynndrum-implementation.md).

## 1. Summary

LynnDrum's DSP core is the original firmware (global-state C) wrapped for a plugin. webdsp is architecturally stronger in
most structural areas. What LXR offers is mainly engineering discipline, plus two measured webdsp defects that an
LXR-style catalogue approach exposed. Neither defect came from reading LXR's DSP.

LynnDrum does better:
1. Test discipline: tiers T0 bit-exact / T1 registered deviation / T2 metric tolerance; "no deviation without a test";
   sweeps generated from the parameter table; an instance-isolation test (`Tools/compare/test_instances.c`).
2. Delay time changes: a 10 ms crossfade between two read heads (`Delay.c`, `Delay_setDelayTime`).
3. One parameter source: `Tools/params/params.json` generates host params, UI metadata and docs.

webdsp does better:
4. Modulation keeps `base` separate from `effective` (`modulation.h`). LXR rewrites live parameters every block through its
   MIDI CC handler and restores them; its `DEVIATIONS.md` records that the base/effective split was never built.
5. Instance-owned state (LXR: thread-local "current engine" plus identifier macros).
6. Sample-accurate in-block events (LXR quantises to 64-frame blocks).
7. Bus graph (topological order, cycle guard, post-fader sends, glides) and master limiter/meters (LXR: one FX bus,
   binary routing, no send level, saturating int16 sum).
8. Retrigger smoothness for kick/bass/fm (LXR resets phase unconditionally: the "only if envelope closed" guard is
   compiled out because `USE_AMP_FILTER` is undefined).

Equivalent: mono statically owned voice per track; linear-in-Hz pitch sweep (`f = tune*(1 + extra*env)`) in kick, tom,
percsynth.

Correction to LynnDrum's own `lxr-dsp-lessons.md`: it describes a nonlinear ZDF filter, but `ENABLE_NONLINEAR_INTEGRATORS`
is 0. The shipping filter is a linear ZDF SVF with an input soft-clip, a soft-clip on state `s1`, and `fastTanh` on the
LP output.

## 2. Architectures

```
LynnDrum / LXR
 host / UI / sequencer --> midiParser_ccHandler (7-bit CC switch; also the modulation write-back path)
                               | writes voice-struct globals
 ======= block boundary: 64 frames @ 44 140 Hz =======
 mixer_calcNextSampleBlock:
   resetTargets -> velocity mod -> 6 LFOs (via ccHandler) -> SVF coeffs -> EGs (control rate)
   per voice: osc(+mod osc) -> transient -> SVF -> amp EG (block lerp) -> vel -> shaper -> fader 1-pole
              -pan (latched at trigger)-> route [6 outputs | FX bus]
   FxBus (exactly one type) -> int16 saturating sum -> output
```

```
webdsp
 client -> runtime API -> command (between blocks) -> Engine (owns everything, instance state)
 Engine::process (<=512 frames):
   scheduled notes / transport / param events -> ramps
   -> voices + instruments (per-sample; 32-frame chunks only while an LFO targets them)
   -> bus accumulators -> buses in topological order [FxChain -> gain -> pan -> main*g | send*g]
   -> master bus -> capture tap -> limiter -> meters
```

| Problem | LynnDrum | webdsp | Stronger |
|---|---|---|---|
| State ownership | Globals + `lxr_cur` macros | Instance members | webdsp |
| Control/audio split | Control tick per 64 frames | Per-sample EGs; 32-frame chunks while modulating (10-57 ns/frame per instrument, `realtime-audit.md`) | Different; no change |
| Parameter path | CC switch, ranges implied by enum | Typed setters, clamped, self-describing tables | webdsp |
| Voice lifecycle | Hard reset on trigger, SVF zeroed | `fresh` clears state, retrigger carries it, `noteOnRestart` 2 ms fade | webdsp, but inconsistent (finding B) |
| Event timing | Block-quantised | Sample-accurate delay/duration | webdsp |
| FX / routing | One FX type, binary routing | Ordered chains, graph, sends | webdsp |

## 3. DSP comparison

| Area | LynnDrum | webdsp | Assessment |
|---|---|---|---|
| Oscillators | 32-bit integer phase, mipmapped 1024 tables | Double phase, PolyBLEP, PM FM | webdsp cleaner |
| Envelopes | Rational slope curve `(1+k)x/(1+k\|x\|)`, block rate | Linear ADSR (squared in instruments), `PercEnv`/`ExpEnv` | Different; curve is a feature idea |
| Filter | Linear ZDF SVF | RBJ biquad, double state | Equal (measured, below) |
| Mod rate | 690 Hz, LFO 200 Hz | ~1.5 kHz chunks, 1.5 ms smoothing, octave slew limit | webdsp |
| Delay | int16 feedback, 10 ms two-head crossfade | `Delay` jumps; `DubDelay` slews | finding A |
| Compressor | dB-domain one-pole, averaged L+R | Soft knee, stereo-linked by max | webdsp |
| Distortion | Rational shaper, diode/triode tables | `tanh` + bias + DC block | Equal |
| Smoothing | Faders/drive 1-pole; pan latched | Bus gain/send/pan glide; instrument params raw | webdsp at bus level; instrument level unmeasured |
| Master | Saturating add | Limiter + meters | webdsp |

Filter measurement: a 110 Hz saw swept 100 to 4000 Hz in 50 ms, cutoff updated every 16/32/128 samples, Q 0.7 to 15. The
biquad's error against its own per-sample update was equal to or lower than a TPT SVF's (Q 9.7 every 16 samples: 0.058
vs 0.066). There is no modulation-robustness case for swapping filters.

## 4. Problems exposed

### A. `Delay` time changes click
- `delay.h` recomputes `delayFrames` from `delaySeconds_` and `setParam` applies it at once, so the read pointer jumps.
  `fx_params.h` pins `time` as non-modulatable for this reason, but knob drags and `scheduleParam` ramps still reach it.
  webseq (`fxChain.ts`) and webtracker (`fxParams.js`) both expose it as a knob.
- Measured: a 100 ms time ramp in 128-frame steps gave a max sample step of 0.381 vs 0.035 steady (11x). A single
  250 to 300 ms jump gave 2.4x the input slope.
- LXR's two-head crossfade measured 1.0x in a re-implementation. `DubDelay`'s slew (<=0.4 frames/sample) measured 0.8x.

### B. Retrigger continuity is a per-instrument choice that nothing checks
- `noteOnRestart`: bass, edge, fm, percsynth, pwmbeast. Plain `noteOn`: hypersaw, pulse, pad, kick. Digital machines use
  `ExpEnv` triggers.
- Measured: retriggering a ringing `dm_thump` 21 ms in moved the output -0.391 to -0.280 to -0.030 in two samples
  (previous step under 0.001). Cause: the FM-index envelope restarts with a 0.1 ms attack (`dm_thump.h` `noteOn`, reached
  via `digital_machine.h`). kick, bass and fm were continuous.
- The other four DM machines are probably affected, but noise/metal content makes the step metric uninformative for them.

## 5. Checked and fine

- `Engine::process`: 0 allocations over 3000 blocks of a busy scenario (18 instruments, 10 FX kinds on 8 buses, sends,
  routing, ramps, choke groups).
- Construction cost: Delay 25 us, DubDelay 49 us, a 4-effect chain ~200 us vs a 2.7 ms quantum. WASM `memory.grow` cost
  is unmeasured.
- Parameter sweeps: 1029 instrument cases (all 18 kinds; min/mid/max per parameter, all-min/max/mid, 12 random vectors,
  velocities 0/0.2/1/2) and 320 FX cases with noise input: no non-finite samples, no hung voices, peak < 3.
- C++/TS enum mirror: 24 enums parsed, 0 mismatches.
- `webvst` sets `ScopedNoDenormals`.

## 6. DSP ideas (ranked)

- High value: none new (Delay smoothing is finding A).
- Worth experimenting: a rational slope curve as one primitive for envelope curvature and tapers (`adsr-envelope.md`
  lists curve shapes as not available); a start-phase "offset" control for kick transients (we always start at the peak).
- Low priority: FM depth tied to the pitch envelope; table-based waveshaper curves; tuned S&H noise as FM source.
- Not worth porting: ZDF SVF as a replacement, mipmapped tables, integer phase, block-rate modulation, LXR's compressor.

## 7. Architecture lessons

| LXR practice | Class | Verdict |
|---|---|---|
| T0/T1/T2 tiers, "no deviation without a test" | Architectural pattern | Adopt the vocabulary and the sweep idea |
| Parameters generated from one JSON | Architectural pattern | Optional; a parse check is enough |
| Thin C API, no host types in the core | Architectural pattern | Already have it |
| Solo/mute policy in the bridge, engine sees one boolean | Architectural pattern | Already split (BusGain / BusMainGain) |
| Block-rate control tick | Embedded optimisation | Skip |
| int16 buses, QADD16, ITCM/DTCM, fp16 buffer | Hardware-specific | Skip |
| Fixed 44 140 Hz / 64-frame block | LXR-specific | Skip |
| Mute as "stop triggering" (tails ring) | LXR-specific | Different semantics from our BusGain mute |

## 8. Final answer

Steal LXR's test discipline (oracle, tiers, table-generated sweeps, instance isolation) and its habit of registering every
deliberate deviation with a test. Leave behind global state and macro tricks, parameters fused to a protocol handler,
per-voice copy-paste divergence, block-stepped modulation, fixed rate/block, unconditional phase resets. Experiment first
with the Delay slew and the all-kinds retrigger/parameter catalogue, then the slope-curve A/B.

Limits: native probes only; no listening; four DM machines not conclusively checked at the retrigger boundary; Saturation
drive smoothing unmeasured; the master limiter passes NaN (`std::max` with NaN returns the other argument) but no source
of NaN was found.
