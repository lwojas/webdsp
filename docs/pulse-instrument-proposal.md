# Pulse — rhythmic synth proposal

Status: **implemented** (stages 1-6; see [`pulse-instrument.md`](./pulse-instrument.md) for shipped behaviour). Stage 7, client integration, is open. Listening gate (stage 4) is still to be judged by ear. Follows the shape of
[`bass-instrument-proposal.md`](./bass-instrument-proposal.md). Approved decisions: engine-level
transport (stage 1), transport-aligned modulation, this document.

## 1. Inspiration (what is actually known)

Korg Gadget's **Brussels** is described by Korg as a *monophonic "anthem" synthesizer* with "a
simple and carefully considered parameter structure", delivering "thick unison tones" for
leads. Its user manual lists these controls and nothing more specific:

| Group | Control | Manual text |
|---|---|---|
| Performer | PUMP | "sets the depth of the pumping effect" |
| Performer | REPEAT | "sets the depth of the repeat effect" |
| Generator | BOTTOM | sub-oscillator volume |
| Generator | GLIDE | pitch glide speed |
| Generator | DIFFUSE | spread of the sound |
| Filter | cutoff, TWIST | cutoff; "modulation depth for the oscillator" |
| Output / Reverb | LEVEL; TONE, LEVEL | |

What this tells us: pump and repeat are **single-depth "performer" macros**, not LFO panels,
and the sound underneath is a fat unison/sub lead, not a thin oscillator. The manual does not
document the curve shapes, rates, sync, or what Repeat modulates; those are *our* design
choices below (Repeat → filter, per the project brief). We are not reproducing Brussels.

Sources: Korg Gadget PlayStation user manual (detune.co.jp/KORG_Gadget_PS_User_Manual_en.html),
Korg Gadget collection page (korg.com/uk/products/software/korg_gadget/gadget_collection.php).

## 2. What webdsp already provides

| Area | Finding | Consequence |
|---|---|---|
| Source / filter | `bass.h`: PolyBLEP saw, asymmetric tanh, 24 dB `BiquadFilter`, Q compensation, 16-frame control rate | Reuse the pattern |
| ADSR | `AdsrEnvelope`: allocation-free, always reaches 0, retrigger-continuous | Reuse as-is |
| Lifecycle | `PercCore`, mono per instance, engine-side timed note-off (`duration`) | Reuse as-is |
| **Tempo / beat** | **None.** Engine knows absolute frames only; `stepDurationSeconds` lives in `LookaheadPlayer` (JS) | Needs a small core transport (§4) |
| Frame clock in instruments | `Instrument::render` has no frame position; idle instances are skipped | Beat phase cannot be self-maintained |

## 3. Key decision: transport-aligned, not note-triggered

A pump restarted at note-on is just another per-note decay envelope, which the ADSR already
is. Pump's value is that it keeps ducking on the beat regardless of the notes (syncopation,
sustained notes, chords). So pump **and** repeat are locked to the transport grid.

- Sustained notes breathe without automation.
- Several instances (a chord) pump identically — the grid is engine-wide.
- Retrigger resets neither behaviour.
- No transport set → engine default 120 BPM, anchor frame 0.

Trade-off: an on-beat note starts inside a duck (the classic sidechain sound). To be judged by
ear; mitigation is an internal few-ms phase lead, not a user control.

## 4. Timing boundary (core addition)

```text
client: BPM + frame of a beat --> Engine Transport{anchorFrame, framesPerBeat}  (scheduled at atFrame)
engine, per block:  inst->syncTransport(blockStartFrame, anchorFrame, framesPerBeat)   // default no-op
instrument:         phase = frac((frame - anchor) / period),  period = framesPerBeat * 4 / n
```

- New ABI `webdsp_set_transport(atFrame, anchorFrame, bpm)`; TS `runtime.setTempo(bpm, {atTime?})`.
- Goes through the existing `EventQueue` template so a tempo change lands exactly where the
  lookahead-scheduled notes do (immediate application would misalign by up to the lookahead).
- `Instrument::syncTransport` is an additive non-pure virtual; existing instruments untouched.
  The timed-note-off block split calls it again at the split frame.
- Engine knows a beat grid, not patterns, steps or tracks. Subdivision stays instrument-side.
- On tempo change the client anchors the new tempo at a beat boundary so phase is continuous.

## 5. Candidates

| | **A. Duck & Breathe (chosen)** | B. Note-triggered shaper | C. Groove patterns |
|---|---|---|---|
| Idea | Grid-locked duck curve + filter-repeat contour peaking as the duck recovers | Same curves, phase 0 at note-on, client resolves Hz | `Groove` macro selects 1-bar accent patterns |
| Core change | Transport (~40 lines) | None | Transport + bar/meter |
| Character | Sidechain breathing, coherent across instances | "Bass with a decay" | Most distinctive; sequencer-in-a-synth |
| Cost | Low | Low | Low |
| Complexity | M | S | L |
| Weakness | Needs transport; on-beat transient ducked | Pump redundant with ADSR | Pattern data model / UI scope creep |

## 6. Chosen design (A)

```text
3x saw (PolyBLEP, detuned by diffuse) + sub (-1 oct, fixed level)
        -> asym tanh, drive = base + repeat contour       (pre-filter)
        -> DC block
        -> BiquadFilter LP 24 dB
             cutoff = base * 2^(octs * repeat * r(phase) * velAccent), floor 0.9*f0
             Q = f(resonance), level-compensated
        -> x ADSR^2 x velocity x pump(phase)
        -> soft clip (bounded +-1) -> Panner -> bus
```

Placement rationale:

- **Pump at the gain stage**, after filter and saturation: before the filter a resonant filter
  smears the duck; after the saturator the duck stays clean.
- **Gain is a product** (`ADSR^2 x velocity x pump`), so the envelopes cannot fight and
  `pump = 0` is exactly the ADSR.
- **Repeat is unipolar and upward** from the base cutoff, so the fundamental is never lost; drive
  rides the same contour (brighter *and* grittier at each repeat) — this coupling is the
  character, not an LFO.
- **Diffuse + sub** carry the Brussels-style fat lead body; without them it is "oscillator +
  filter + two LFOs".
- Decay/sustain are private (0 / 1): effectively attack/release. Exposing sustain is cheap if
  it sounds thin. Glide and Twist are deliberately left out of v1.

### Curves (closed-form functions of phase, unit-testable)

- **Pump:** `c(p) = (1 - p/rho)^2` for `p < rho`, else 0; gain `= 1 - depth*c`.
  ~1.5 ms ramp-in at the hit; 1-2 ms one-pole slew on gain (covers rate changes);
  `rho = 0.35 + 0.5*amount` (more pump = deeper *and* longer recovery from one knob);
  minimum gain ~0.03 so it never fully gates.
- **Repeat:** `r(p) = x*e^(1-x)`, `x = p/0.2` — peaks 1 at p = 0.2 as the pump recovers, smooth
  fall, click-free, up to ~3 octaves.
- **Velocity** scales repeat opening (0.5 + 0.5v): sequencer velocities become per-step
  rhythmic emphasis for free.

## 7. Public API

`InstrumentKind::Pulse = 6`, id `"pulse"`, name "Pulse" (working name), category `"synth"`.

| Param | Range | Default | Meaning |
|---|---|---|---|
| tune | 27.5-440 Hz | 110 | Root; note pitch on top |
| cutoff | 60-6000 Hz | 500 | Resting tone, keytracked |
| resonance | 0-1 | 0.25 | Q, compensated, capped ~8 |
| **pump** | 0-1 | 0.5 | Depth and recovery length (one macro) |
| **repeat** | 0-1 | 0.4 | Filter opening + drive per repeat |
| **rate** | 0-5, stepped | 2 | 1/1, 1/2, 1/4, 1/8, 1/16, 1/32 |
| diffuse | 0-1 | 0.3 | Unison spread |
| attack | 1 ms-0.25 s | 3 ms | |
| release | 10 ms-1.5 s | 80 ms | |

Not exposed: LFO shape/destination/phase/curve, sync mode, separate pump/repeat rates,
oscillator internals. Only new ABI: `webdsp_set_transport`.

## 8. Performance

~80 flops/sample (3 PolyBLEP saws, 1 sub sine, tanh, 2 biquads, ADSR, pump slew) ~ 4 MFLOPS
per instance at 48 kHz; control-rate `exp/pow/tanh` every 16 frames. No audio-thread
allocation; transport change is one queue push from the message thread.

## 9. Tests (`native/test/pulse_test.cpp`, bass-test harness)

- **DSP:** pump curve monotone/bounded, depth at p = 0; repeat contour in 0..1, peak at 0.2;
  ADSR timing; `pump = 0, repeat = 0` equals a pure-ADSR render.
- **Musical:** RMS minima spaced `period +- 1 frame` per rate; measured depth within 5% of
  `1 - depth`; high-band energy modulated at the period with repeat > 0, flat at 0; scheduled
  tempo change gives the new period at the right frame; instances started at different times
  duck at identical frames; renders identical across tempos when pump = repeat = 0.
- **Safety:** max resonance/repeat/pump over all rates for 5 s: finite, |y| <= 1, DC mean < eps;
  zero allocations (`operator new` counter); TS `paramIds`; WASM `smoke.mjs`.
- WAV dumps via `PULSE_WAV_DIR` for listening.

## 10. Staged plan

1. Transport core: engine state, scheduled `set_transport`, `Instrument::syncTransport`, ABI,
   TS `setTempo`, tests (useful on its own).
2. `dsp/rhythm.h`: period, pump curve, repeat contour as pure functions + tests.
3. `instruments/pulse.h`, registry/params/kind entries.
4. Tests + WAV fixtures; **listening gate** (distinctive, or bass-with-tremolo?).
5. `npm run build:wasm` and `build:lib`; commit generated `engine.js` and `lib/`.
6. `docs/pulse-instrument.md`, `ARCHITECTURE.md` "Transport" section.
7. Clients later (webtracker: `setTempo`, `duration`, velocity).

## 11. Risks to settle by ear

- Ducked on-beat transients (fix: internal phase lead).
- One shared rate keeps pump/repeat coherent; smallest escape hatch is a "repeat x2" toggle.
- Whether Brussels-like thickness needs more than 3 saws + sub (try before adding Twist).
