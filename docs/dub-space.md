# Dub Space

`EffectKind.DubSpace` (kind 12, `native/src/dsp/dub_space.h`): a tape-style dub echo and a deep, dark
synthetic space that sits **in the echo's feedback loop**, so a dry chord becomes a distinct echo,
then a filtered echo, a smeared echo, a spatial tail and finally a deep cloud. Not a delay followed
by a reverb, and not a clone of any hardware.

```
in --*gE--> [ring] -> tape LP -> saturation -> HP --> x ------------+--> (1-d)*x ---+
       ^                                               |            |               |
       |                                    (gS)       v            |               v
       |                       [smear: 3 modulated allpasses/side]-> s   wet = (1-d)x + d(0.15..0.5 s + t)
       |                                               |   |                         |
       |   r = (1-d)*x + d*s  <------------------------+   +-> [8-line FDN, dark] -> t
       +---- feedback * r                                                   out = dry*(1-mix) + wet*mix
```

## Parameters (`NodeParam`, ids 82..91)

| name | id | range | default | notes |
| --- | --- | --- | --- | --- |
| `echoOn` | `DubSpaceEchoOn` 82 | 0 / 1 | 1 | step, not modulatable |
| `spaceOn` | `DubSpaceSpaceOn` 83 | 0 / 1 | 1 | step, not modulatable |
| `time` | `DubSpaceTime` 84 | 0..9 | 2 (1/8 dotted) | `DubDelayDivision` index of the transport beat; step |
| `feedback` | `DubSpaceFeedback` 85 | 0..0.92 | 0.55 | |
| `tone` | `DubSpaceTone` 86 | 200..16000 Hz | 4000 | low-pass in the loop (Character lowers it up to half) |
| `size` | `DubSpaceSize` 87 | 0..1 | 0.5 | FDN line lengths x0.6..2.8, smear lengths x0.5..2, T60 1.2 s .. 30 s |
| `depth` | `DubSpaceDepth` 88 | 0..1 | 0.5 | share of each repeat that goes through the space |
| `damping` | `DubSpaceDamping` 89 | 0..1 | 0.5 | darkness of the space: line low-pass 6 kHz .. 480 Hz |
| `character` | `DubSpaceCharacter` 90 | 0..1 | 0.3 | see below |
| `mix` | `DubSpaceMix` 91 | 0..1 | 0 | 0 = bypass (clears the tails), like every FX |

## The four configurations

| Echo | Space | Result |
| --- | --- | --- |
| off | off | exact bypass; state cleared once, nothing runs |
| on | off | conventional dub echo: LP + saturation + HP in the loop, tape wow/flutter |
| off | on | the input feeds the space directly: deep synthetic reverb, no repeats. `depth` is its level (floor 0.4) |
| on | on | the space is in the regenerative path (below) |

Each section's send ramps in ~30 ms when switched, and both-off ramps to the dry signal, so toggling
on a running signal does not click. Echo-off kills the echo within that ramp (it is not a ring-out).

## Feedback path

Per pass: read (interpolated, with tape drift) -> one-pole tape LP -> saturator -> 70..200 Hz high-pass
-> **smear** (3 modulated allpasses per side, blended in by `depth`) -> x feedback -> write.
Repeat n therefore keeps `(1-depth)^n` of its defined echo; the rest has been darkened, saturated,
smeared and decorrelated n times. Every repeat also feeds the FDN, so the cloud under the echoes
builds as they recirculate.

**Why the loop holds the smear and not the whole reverb.** A feedback delay network has very sharp
resonant peaks (tens of dB above its average level at a mode of a long tail). A loop that
re-injected it would howl at the strongest mode whatever the feedback. Allpasses have unity gain at
every frequency, so the loop gain is at most `feedback` (<= 0.92) by construction; the saturator
bounds the level on top. The FDN is a branch off the loop (fed by every repeat, heard, not
recirculated). `native/test/dub_space_test.cpp` drives worst-case settings with sustained sines and
noise and checks the tail decays.

## Character

One control for the whole interaction, 0 = relatively clean, 1 = degraded but usable:
tape wow/flutter depth (1.5 -> 21 samples), saturation drive 1 -> 3.5 with a little bias (even
harmonics), loop low-pass x1 -> x0.5, loop high-pass 70 -> 200 Hz, smear allpass gain 0.45 -> 0.72
and drift, FDN modulation depth (the underwater movement), FDN damping x1 -> x0.65.

## Time

Same divisions and tempo follow as `DubDelay` (`setTempo` from the transport). A change glides the
read position with an eased, rate-limited approach (max +-30% pitch), so moving Time on a running
signal bends the repeats like a tape speed change. Mix, feedback, tone, size, depth, damping and
character are smoothed (20 ms; size 350 ms, which also glides the space's pitch).

## Stereo

The echo is dual-mono with decorrelated wow/flutter (right is out of phase). The space is true stereo:
L feeds the even FDN lines and R the odd ones, the outputs are two different orthogonal taps, and the
smear lengths differ per side. A mono source becomes a wide cloud; a mono bus (1 channel) works and
reads the left tap.

## Limits

4 s buffer per channel (about 1.5 MB), like `DubDelay`; no allocation after construction. About
8 reads + 6 modulated allpass reads per sample per frame. The space has no pre-delay or early
reflections of its own (the smear is the onset). A hardware-style "ring out when switched off" is not
modelled.

Tests: `native/test/dub_space_test.cpp`, `test/paramIds.test.ts`, `native/test/smoke.mjs`.
