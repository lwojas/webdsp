# Instrument concepts and ranking

Ideas, not commitments. Each follows the existing model: one monophonic machine, a handful of
musical controls, DSP private. Numbers like `#10` refer to [`techniques.md`](./techniques.md).
Complexity is relative to the existing Kick (≈150 lines): **S** about the same, **M** 2–3×,
**L** needs new infrastructure.

## Concepts

| Concept | Character | Technique | Public controls | Cx |
|---|---|---|---|---|
| **Metal Percussion** (hat/cowbell/cymbal) | Bright, inharmonic, short or open | #15 square cluster, optionally #5 / #10 | tune, decay, tone, open | S |
| **Modal Percussion** (tom/block/bar/bell) | Struck object, wood→metal | #10 modal bank + #13 tension | tune, material, size, strike, decay | M |
| **Snare** | Snappy electronic→full | #16 | tune, snap, body/noise, decay, drive | S |
| **808 Sub Kick** | Round, long, rubbery | #14 | tune, decay, click, warmth | S |
| **Pluck Bass** | Plucked, damped, fat with drive | #11 + #6 | tune, pluck, decay, damping, drive | M |
| **PD/FM Bass** | Plucky digital bite, snappy | #3 or #4 with index envelope | tune, brightness, snap, decay | S |
| **Fold Bass / Zap** | Glassy sub with sparkle | #7 (+#9) | tune, fold, strike, decay | M |
| **Ping Resonator** (zap/laser/acid-ish) | Self-oscillating, aggressive | #8 | pitch, ring, grit, excite | S |
| **Analogue Lead** | Clean saw/pulse with drift | #1 + #19 + `BiquadFilter` | tune, pulse, drift, brightness | S |
| **Shaker/Noise Percussion** | Textures, shakes | #17 | pitch, shake, decay, brightness | S |

Note: Lead/Bass concepts imply sustained notes; the contract supports `instrumentNoteOff`
but polyphony is separate instances. Prefer percussive/plucked concepts first.

## Ranking

Qualitative; "fit" = reuse of existing webdsp code + clean instrument boundary.

### High priority (where to look tomorrow)

| Technique | Musical potential | Complexity | CPU | webdsp fit | Instrument potential |
|---|---|---|---|---|---|
| #10 Modal bank (+#13 tension) | High | Medium | Low | Good (BiquadFilter / new 2-pole) | Toms, blocks, bells, metal, bars |
| #15 + #5 metallic cluster / loopback FM | High | Low | Low | Excellent | Hats, cowbell, cymbal, gong |
| #11 Karplus-Strong pluck | High | Low–Med | Low | Good (Delay, Saturation) | Pluck bass, harp, muted string |
| #4 FM/PM with index envelope | High | Low | Low | Good | Bells, e-piano, FM bass, metallic hits |
| #3 Phase distortion | Medium–High | Low | Low | Good | Plucky bass, lead; distinct from Kick |

### Secondary

| Technique | Why interesting | Caveat |
|---|---|---|
| #7 Wavefolder (+#9 ADAA) | Distinctive west-coast tone | Needs antialiasing effort |
| #8 Nonlinear feedback ping | Cheap, aggressive, characterful | Stability tuning |
| #14 Bridged-T kick | Alternate kick personality | Overlaps Kick; behavioural approximation only |
| #1/#2 BLEP/DPW saw/pulse | Needed for any leads/basses | Is a primitive, not an instrument by itself |
| #19 Drift | Cheap realism, reusable across instruments | Easy to overdo |

### Not worth pursuing now

#20 RDFT drum resynthesis, #12 commuted synthesis (needs excitation data), #18 full friction
models (stability; keep as a simple loop at most), GPU modal banks, finite-difference
membrane/plate simulations, wave-digital circuit solvers.

## Recommendation (not established fact)

Build **Metal Percussion** next (techniques #15 with an optional #5 loopback-FM "gong" mode):
smallest step from Kick, covers a gap in the drum set, reuses `BiquadFilter`/`AdsrEnvelope`/
`Bitcrusher`, and answers whether a second machine reveals shared primitives (sine/square
oscillator, exponential decay) worth extracting. Then **Modal Percussion** (#10) as the first
resonator-based machine; its bank becomes the base for toms, bells and pluck variants.

Agent checklist before coding: pick the concept → read its technique entries → decide the 4–6
controls → write `native/src/instruments/<name>.h` and the docs page as per
[`../instruments.md`](../instruments.md#adding-a-new-instrument).

---

## Sustained & melodic concepts (added)

| Concept | Character | Technique | Public controls | Cx |
|---|---|---|---|---|
| **Supersaw Pad/Lead** | Wide, shimmering stack | #21 (+#1, #28) | tune, spread, mix, brightness, attack, release | M |
| **Sync Lead** | Ripping, sweeping mono lead | #22 (+#1) | tune, sync, sweep, pulse, decay | M |
| **Wavetable Voice** | Morphing digital/organic | #23 | tune, morph, motion, brightness | M |
| **Organ** | Drawbar organ with click | #24 | tune, body, bright, click, drive | S |
| **Choir/Vowel** | Vowel pad or talking bass | #25 | tune, vowel, breath, vibrato | M |
| **Bowed String** | Sustained, expressive | #26 | pitch, pressure, speed, brightness | M–L |
| **Moog Bass/Lead** | Fat mono, squelchy | #27 (+#1) | tune, cutoff, resonance, env, drive | M |

Sustained instruments must define note-off behaviour (short release, click-free) and
hold their LFO/phase state deterministically; chords require several instances.

### Ranking among sustained voices

| Technique | Musical potential | Complexity | CPU | webdsp fit | Instrument potential |
|---|---|---|---|---|---|
| #27 Subtractive + nonlinear ladder | High | Medium | Low–Med | Good (needs ladder) | Mono bass/lead, most expected sound |
| #21 Supersaw | High | Medium | Medium | Good | Pads, trance leads |
| #25 Formant (bandpass-over-saw) | Medium–High | Low–Med | Low | Good (`BiquadFilter`) | Distinctive pad/vowel bass |
| #24 Additive organ | Medium | Low | Low | Good | Organ, glass pads |
| #26 Bowed string | High | High | Medium | Moderate | Expressive strings; riskiest |
| #23 Wavetable / #22 Sync | Medium | Medium | Low | Good | Morphing leads, sync sweeps |

**Recommendation:** for a first sustained instrument, build **Moog Bass/Lead** (#27) or
**Supersaw** (#21); both rely on #1 (PolyBLEP), which is the primitive to write once and
reuse. Defer #26 until after the Karplus-Strong pluck (#11) exists.
