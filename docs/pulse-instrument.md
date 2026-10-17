# Pulse

A compact rhythmic synth: a fat saw/sub tone whose identity is **pump** (rhythmic ducking) and
**repeat** (rhythmic filter opening, depth set by `filterMod`, plus drive), both locked to the tempo grid. Design rationale and
alternatives: [`pulse-instrument-proposal.md`](./pulse-instrument-proposal.md). Client integration: [`pulse-client-guide.md`](./pulse-client-guide.md). Source:
`native/src/instruments/pulse.h`, `native/src/dsp/rhythm.h`; tests: `native/test/pulse_test.cpp`.

## Signal flow

```text
3x saw (PolyBLEP, detuned by diffuse, up to +-20 cents) + sub sine (-1 oct, 0.35)
  -> asymmetric tanh, drive = 0.25 + 0.45 * repeat * contour * accent     (pre-filter)
  -> DC block (5 Hz) -> BiquadFilter LP 24 dB
       cutoff = cutoff * keytrack * 2^(4 oct * filterMod * contour * accent), floor 0.9 * f0
  -> x amp env^2 x velocity -> soft clip -> x pump gain -> Panner -> bus
```

The repeat contour reaches the filter through the shared [filter modulation](./filter-modulation.md) path (unipolar signal, depth = `filterMod` x 4 oct x accent). `repeat` drives only the saturation.

`accent = 0.5 + 0.5 * velocity`, so sequenced velocities become per-step emphasis on the repeat.
Mono per instance; a fresh note resets oscillator and filter state, a retrigger carries it.

## Parameters (`PulseParam`, `InstrumentKind.Pulse = 6`, id `pulse`, category `synth`)

| Param | Range | Default | Meaning |
|---|---|---|---|
| tune | 27.5-440 Hz | 110 | Root; note `pitch` is semitones on top |
| cutoff | 60-6000 Hz | 500 | Resting low-pass cutoff, keytracked |
| resonance | 0-1 | 0.25 | Q 0.71..~8, level-compensated |
| pump | 0-1 | 0.5 | Duck depth (0 to -30 dB, so 0.5 = -15 dB; never a full gate) and recovery length |
| repeat | 0-1 | 0.4 | Drive (grit) per cycle |
| filterMod | 0-1 | 0.3 | Standard filter modulation depth: how far each repeat cycle opens the filter (up to 4 octaves). Independent of `repeat`; 0 = static filter |
| rate | 0-5 step | 2 | 1/1, 1/2, 1/4, 1/8, 1/16, 1/32 of a 4/4 bar (rounded) |
| diffuse | 0-1 | 0.3 | Unison spread |
| attack | 1 ms-0.25 s | 3 ms | |
| release | 10 ms-1.5 s | 80 ms | |

Decay/sustain are private (0 / 1): the pump supplies the dynamics.

## Behaviour

- **Grid, not note.** Phase = `frac((frame - anchor) / period)`, `period = framesPerBeat * 4 / n`.
  Pump and repeat hit on the grid whether the note started on it or not, through sustained notes
  and retriggers; several instances pump together.
- **Pump** gain = `-30 dB * pump * duck(p)` (in dB), `duck = (1 - p/recovery)^2`, `recovery = 0.35 + 0.5 *
  pump`. A 1.5 ms one-pole slew gives the hit a click-free ramp-in and smooths rate/tempo changes.
  `pump = 0` is exactly 1.
- **Legato re-articulation.** A note landing on one still held (the tracker's default: each note
  lasts until the next) would find the envelope already at full level, so attack and release would
  never be heard on a sequenced line. Instead the new note's gain restarts below full level and
  climbs over `attack`; the dip scales with attack (3 ms barely dips, >= 30 ms is a full
  re-attack) and a 0.7 ms slew keeps it click-free. **Release** only sounds where there is a gap
  (a note shorter than its step, or a note-off): in a legato line the next note takes over, so
  there is no tail to shape.
- **Repeat** contour (the shared signal for both `repeat`'s drive and `filterMod`'s cutoff): 0 at the hit, peak 1 at p = 0.2 (as the pump recovers), back to exactly 0
  at the cycle end. Unipolar: the filter only opens above `cutoff`.
- **Pump is the last stage**, after filter, ADSR and soft clip: it multiplies the finished signal
  by a 0..1 gain (it only ducks, never boosts). It never feeds the envelope, so ADSR and pump are
  independent (the pump phase comes from the transport, the envelope from the note), `pump = 0` is
  exactly the plain signal, and the duck has its stated depth even when the clipper is working.

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
returns to base; filterMod = 0 leaves the filter at base (even with pump on); filterMod and repeat are independent; repeat repeats on
every cycle of a sustained note, through the release tail and after a tempo change; repeat does
not change the amp-ADSR lifetime; the pump ratio is identical with repeat on or off; mid-note tempo change; off-grid note and retrigger duck on the same frames;
duration/release; max-setting stability (finite, <= 1, no DC); determinism; zero audio-thread
allocations. `PULSE_WAV_DIR=<dir> npm run test:native` dumps listening fixtures (pump, repeat,
tempo change, stress, a 124 BPM groove).
