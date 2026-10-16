# Pulse

A compact rhythmic synth: a fat saw/sub tone whose identity is **pump** (rhythmic ducking) and
**repeat** (rhythmic filter opening), both locked to the tempo grid. Design rationale and
alternatives: [`pulse-instrument-proposal.md`](./pulse-instrument-proposal.md). Source:
`native/src/instruments/pulse.h`, `native/src/dsp/rhythm.h`; tests: `native/test/pulse_test.cpp`.

## Signal flow

```text
3x saw (PolyBLEP, detuned by diffuse, up to +-20 cents) + sub sine (-1 oct, 0.35)
  -> asymmetric tanh, drive = 0.25 + 0.45 * repeat contour * accent     (pre-filter)
  -> DC block (5 Hz) -> BiquadFilter LP 24 dB
       cutoff = cutoff * keytrack * 2^(3 oct * repeat * contour * accent), floor 0.9 * f0
  -> x amp env^2 x velocity x pump gain -> soft clip -> Panner -> bus
```

`accent = 0.5 + 0.5 * velocity`, so sequenced velocities become per-step emphasis on the repeat.
Mono per instance; a fresh note resets oscillator and filter state, a retrigger carries it.

## Parameters (`PulseParam`, `InstrumentKind.Pulse = 6`, id `pulse`, category `synth`)

| Param | Range | Default | Meaning |
|---|---|---|---|
| tune | 27.5-440 Hz | 110 | Root; note `pitch` is semitones on top |
| cutoff | 60-6000 Hz | 500 | Resting low-pass cutoff, keytracked |
| resonance | 0-1 | 0.25 | Q 0.71..~8, level-compensated |
| pump | 0-1 | 0.5 | Duck depth (max 97%, never a full gate) and recovery length |
| repeat | 0-1 | 0.4 | Filter opening (up to 3 octaves) and drive per cycle |
| rate | 0-5 step | 2 | 1/1, 1/2, 1/4, 1/8, 1/16, 1/32 of a 4/4 bar (rounded) |
| diffuse | 0-1 | 0.3 | Unison spread |
| attack | 1 ms-0.25 s | 3 ms | |
| release | 10 ms-1.5 s | 80 ms | |

Decay/sustain are private (0 / 1): the pump supplies the dynamics.

## Behaviour

- **Grid, not note.** Phase = `frac((frame - anchor) / period)`, `period = framesPerBeat * 4 / n`.
  Pump and repeat hit on the grid whether the note started on it or not, through sustained notes
  and retriggers; several instances pump together.
- **Pump** gain = `1 - 0.97 * pump * duck(p)`, `duck = (1 - p/recovery)^2`, `recovery = 0.35 + 0.5 *
  pump`. A 1.5 ms one-pole slew gives the hit a click-free ramp-in and smooths rate/tempo changes.
  `pump = 0` is exactly 1.
- **Repeat** contour: 0 at the hit, peak 1 at p = 0.2 (as the pump recovers), back to exactly 0
  at the cycle end. Unipolar: the filter only opens above `cutoff`.
- **Pump is applied after filter and shaper**, multiplied with the ADSR (`ADSR^2 x velocity x
  pump`), so they cannot fight.

## Transport

`AudioRuntime.setTempo(bpm, { time?, anchorTime? })` -> `webdsp_set_transport(atFrame, anchorFrame,
bpm)`. Beats lie at `anchorTime + n * 60/bpm`. Until set: 120 BPM anchored at frame 0, so an
unset client gets a pump that is steady but not aligned to its own steps. Call it with the time of
step 0 (and again, anchored at a beat, on tempo changes). Changes are scheduled like notes and
applied at block granularity (~2.7 ms). See ARCHITECTURE.md, "Transport".

## Client notes

- Pass `duration` on notes (sustained kind); drums omit it.
- Call `setTempo` whenever the tracker's tempo/start changes, before scheduling notes against it.
- Notes landing on a beat start inside a duck (the sidechain sound). If that is too dull by ear the
  planned fix is an internal phase lead, not a control.

## Tests

`native/test/pulse_test.cpp`: curve properties; transport scheduling; pump tracks the grid at all
six rates (pumped/unpumped render ratio vs predicted gain); depth scales with amount; tempo and
rate irrelevant when pump = repeat = 0 (bit-identical); repeat opens the filter on the grid and
returns to base; mid-note tempo change; off-grid note and retrigger duck on the same frames;
duration/release; max-setting stability (finite, <= 1, no DC); determinism; zero audio-thread
allocations. `PULSE_WAV_DIR=<dir> npm run test:native` dumps listening fixtures (pump, repeat,
tempo change, stress, a 124 BPM groove).
