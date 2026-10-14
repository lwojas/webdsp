# Kick instrument (first synthesized instrument)

An experiment answering: *can a small amount of DSP give webdsp a musically useful,
characterful drum voice with no sample?* Answer so far: yes — ~150 lines, mostly reused engine
DSP. Code: `native/src/instruments/kick.h`; contract: `native/src/instrument.h`; tests:
`native/test/kick_test.cpp`.

## Public contract

Generic lifecycle, notes, scheduling, metadata and client integration are covered in
[`instruments.md`](./instruments.md). This page is the Kick-specific part: its controls and
internals. Create it with `runtime.createInstrument(InstrumentKind.Kick, { bus })`.

Kick parameters (`KickParam`):

| id | name | range | default | unit | effect |
|---|---|---|---|---|---|
| 0 | `tune` | 30–120 | 50 | Hz | pitch the body settles on |
| 1 | `pitchDrop` | 0–4 | 2.5 | oct | how far above `tune` the sweep starts |
| 2 | `pitchDecay` | 0.005–0.25 | 0.04 | s | how quickly the sweep falls |
| 3 | `decay` | 0.05–2 | 0.45 | s | body length |
| 4 | `punch` | 0–1 | 0.5 | – | level of the attack click |
| 5 | `drive` | 0–1 | 0.3 | – | saturation amount |

`velocity` scales level linearly; `pan` is −1..+1 (as `VoiceParam.Pan`).

## Internal signal path (private to Kick)

```
sine osc ← pitch env (one-pole decay, linear in Hz: f = tune·(1 + (2^drop − 1)·e))
   + click (deterministic xorshift noise, 2 ms decay → BiquadFilter low-pass 3.5 kHz)  ← punch
   × amp env (AdsrEnvelope: 0.5 ms attack → decay → 0, applied squared)                ← decay
   → Saturation (tanh, asymmetry 0.25·drive, −0.35 dB/dB level trim)                   ← drive
   → Panner → bus accumulator
```

Reused: `AdsrEnvelope`, `BiquadFilter`, `Saturation`, `Panner`, the scheduler queue and the bus
routing. New: the sine oscillator and one-pole decays (the engine ADSR is linear-segment only).
The envelope is squared so the tail is closer to the exponential decay of an analogue
circuit; the saturator's asymmetry adds even harmonics, which is most of the "character".

Design notes found while building it:
- `Saturation` with asymmetry emits a DC-blocker transient even from silence, so the kick keeps
  its pre-note delay frames out of the saturator (tested: exact silence before a scheduled note).
- `Saturation`'s fixed DC blocker (R = 0.995, corner ≈ 38 Hz) slightly softens the lowest
  fundamentals when drive > 0. Acceptable for now; a kick-specific lower-corner blocker is the
  fix if sub-weight matters.
- Retrigger keeps oscillator phase and envelope level and only restarts the sweep/click, so
  there is no discontinuity (tested).
- No oversampling; extreme drive + high tune can alias slightly (same tradeoff as the saturation node).

## Consuming it

See [`instruments.md`](./instruments.md#integrating-in-a-client). Kick-specific: ids are
`KickParam`; a kick is a one-shot, so `instrumentNoteOff` is only needed to choke it early.

## What is Kick-specific vs. reusable

- **Kick-specific:** the signal path above, its six parameters and mappings (e.g. drive → dB
  pre-gain + asymmetry + trim), and all the constants.
- **Intended for future instruments:** the `Instrument` interface (noteOn/noteOff/setParam/
  render/isActive), the self-describing `InstrumentParamInfo` table, `instrument_registry.h`,
  bus routing + scheduling shared with samples, and the reusable DSP building blocks. Each new
  machine (snare, hat, bass…) brings its own header, own parameter table, own sound design.
- **Deliberately not built:** a generic oscillator/filter/envelope toolbox, polyphonic
  instrument instances, per-instrument FX, preset storage.

## Listening / inspecting

```
KICK_WAV_DIR=/some/dir bash -c 'clang++ -std=c++17 -Inative/src -o native/build/kick_test \
  native/test/kick_test.cpp && native/build/kick_test'
```
writes `default.wav`, `punch.wav`, `drive.wav`, `extreme.wav` (48 kHz, 16-bit stereo). Tests run
with the rest of `npm run test:native`; they include a zero-allocation check on the render path.
