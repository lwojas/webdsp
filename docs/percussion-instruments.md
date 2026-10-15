# Percussion instruments: Snare, Clap, Tom, Hi-Hat

Synthesized drums following the [Kick](./kick-instrument.md) precedent and the generic
contract in [`instruments.md`](./instruments.md). Each machine owns its sound design; the
application sees only the controls below. Technique background lives in
[`synth-dsp/`](./synth-dsp/README.md) and is intentionally not repeated here.

Code: `native/src/instruments/{snare,clap,tom,hihat}.h`, shared bits in
`native/src/dsp/percussion.h`. Tests: `native/test/percussion_test.cpp`.

## Identity

| `InstrumentKind` | `id` (stable) | `name` (display) | `category` |
|---|---|---|---|
| `Kick` = 0 | `kick` | Kick | `drum` |
| `Snare` = 1 | `snare` | Snare | `drum` |
| `Clap` = 2 | `clap` | Clap | `drum` |
| `Tom` = 3 | `tom` | Tom | `drum` |
| `HiHat` = 4 | `hihat` | Hi-Hat | `drum` |

`id` is the machine identifier to persist or refer to; `name` is display text and may change.
`category` is a free string for UI grouping (only `drum` exists today). Read all three from
`runtime.getCapabilities().instruments[]` (`{ kind, id, name, category, params }`).

## Note pitch

`instrumentNoteOn(inst, { pitch })` — semitones, offset **on top of** the instrument's `tune`
(0 = tune as set), same convention as a sample event's `pitch`. All five drums honour it;
Tom is the one where it is the point (tom fills from tracker notes).

## Parameters

Ids are per-kind enums (`SnareParam`, `ClapParam`, `TomParam`, `HiHatParam`); ranges/defaults
come from `getInstrumentParams(kind)`. "Decay" everywhere is roughly the time to fall 40 dB.

| Kind | Param | Range | Default | Effect |
|---|---|---|---|---|
| Snare | `tune` | 100–400 Hz | 185 | body pitch |
| | `decay` | 0.05–0.8 s | 0.22 | overall length (body is always shorter than the rattle) |
| | `snap` | 0–1 | 0.5 | opening crack: transient level and noise brightness |
| | `noise` | 0–1 | 0.6 | rattle vs. tonal body balance |
| Clap | `tune` | 600–3000 Hz | 1300 | centre of the noise band |
| | `decay` | 0.05–0.8 s | 0.25 | length of the final tail |
| | `spread` | 0–1 | 0.5 | time between the burst cluster's hits (4–16 ms) |
| | `snap` | 0–1 | 0.5 | sharpness/resonance of the bursts |
| Tom | `tune` | 60–400 Hz | 120 | resting fundamental |
| | `pitchDrop` | 0–2 oct | 0.6 | how far above tune the strike starts |
| | `decay` | 0.1–1.5 s | 0.45 | ring length |
| | `body` | 0–1 | 0.5 | membrane overtones + stick tick |
| | `damp` | 0–1 | 0.3 | muffling: shorter ring, darker tone |
| Hi-Hat | `tune` | −12…+12 st | 0 | shifts the metallic cluster |
| | `decay` | 0.02–1.2 s | 0.08 | short = closed, long = open |
| | `metal` | 0–1 | 0.7 | filtered noise → inharmonic metallic cluster |
| | `tone` | 0–1 | 0.5 | brightness |

## What each one is

- **Snare** — two-mode head (f and 1.83 f, fast pitch drop) with its own short envelope, plus
  band-passed noise on a separate longer envelope and a 3 ms snap burst. Soft-clipped.
- **Clap** — one note is a *cluster*: three 5 ms noise bursts then a `decay`-length tail,
  spaced by `spread`, each with its own gain, through a band-pass plus a lower "room" band.
  The timing lives in the instrument; no sequencer feature needed.
- **Tom** — tuned membrane: sine fundamental with 1.59× and 2.14× modes that die faster, a
  strike pitch drop (linear in Hz, like Kick), stick tick, and a `damp` low-pass.
- **Hi-Hat** — six PolyBLEP square oscillators at the 808's inharmonic frequencies, mixed
  with noise, band-passed and high-passed, with a two-stage (chick + body) envelope. Open and
  closed are the same machine; a short hat retriggering an open one chokes it.

## Shared pieces (`dsp/percussion.h`)

`PercEnv` (exponential decay with a continuous-retrigger attack), `PercNoise` (deterministic
xorshift), `PercBandpass` (RBJ band-pass; the engine's `BiquadFilter` has no band-pass),
`PercCore` (velocity/pitch/pan/pre-note-delay/active state + bus routing) and `setTableParam`.
Existing `BiquadFilter` (Tom damping, Hi-Hat high-pass) and `Panner` are reused. Kick was
left as is apart from honouring note `pitch`.

## Listening

`PERC_WAV_DIR=/some/dir bash -c 'clang++ -std=c++17 -Inative/src -o native/build/percussion_test native/test/percussion_test.cpp && native/build/percussion_test'`
writes one default hit per instrument as a 48 kHz 16-bit WAV.
