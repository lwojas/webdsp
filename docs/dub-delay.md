# Dub Delay

`EffectKind.DubDelay` (kind 9, `native/src/dsp/dub_delay.h`): a tempo-synchronised feedback delay
with a low-pass **inside** the feedback loop, so each repeat is darker than the last. It sits next
to the plain `Delay` (free time in seconds), which is unchanged.

```
in ---+---------------------------> dry*(1-mix) + wet*mix --> out
      |                      ^
      +--> [ring buffer] ----+--> wet
            ^                    |
            +-- feedback * LP <--+
```

## Parameters (`NodeParam`, ids 45..48)

| name | id | range | default | notes |
| --- | --- | --- | --- | --- |
| `division` | `DubDelayDivision` 45 | 0..5 (step) | 2 (1/8 dotted) | `DubDelayDivision` enum; not modulatable |
| `feedback` | `DubDelayFeedback` 46 | 0..0.95 | 0.45 | |
| `tone` | `DubDelayTone` 47 | 200..16000 Hz | 3000 | one-pole low-pass in the loop |
| `mix` | `DubDelayMix` 48 | 0..1 | 0 | 0 = bypass, like every other FX (clears the tail) |

`mix` defaults to 0 because an effect is inert until parameterised (the convention shared by Delay,
Reverb, Phaser...). A client adding a Dub Delay sets the mix it wants (a send bus: 1).

Divisions are counted in sixteenth notes: 1/16 = 1, 1/8 = 2, 1/8 dotted = 3, 1/4 = 4,
1/4 dotted = 6, 1/2 = 8. `delayFrames = framesPerBeat / 4 * sixteenths`. At 120 BPM that is
125 / 250 / 375 / 500 / 750 / 1000 ms.

## Tempo

There is no time parameter. `DSPNode::setTempo(framesPerBeat)` is delivered by `FxChain::setClock`
every block (the same beat grid synced LFOs follow, set by `setTransport` / `scheduleTransport`),
and once when the instance is added. A BPM change therefore moves the delay with no client work.

## Time changes

A new division or tempo does not jump the read pointer. The read position slews toward the target
at a bounded rate (0.4 frames per sample, so repeats bend by at most +-40% in pitch) with linear
interpolation: a tape-style glide rather than a click. Settled, the time is a whole number of
frames, so the interpolation is exact and adds no filtering. A first block (or the first block
after the effect was bypassed) starts at the target with no glide.

The buffer holds 4 s (about 1.5 MB per instance at 48 kHz, allocated when the effect is added).
A division longer than that at a very slow tempo (1/2 below 30 BPM) is halved until it fits, which
keeps it on the beat grid. Mix and feedback are smoothed (~10 ms) so stepped automation does not
zipper.

## Scope

Four controls and nothing else: no modulation, saturation, ping-pong, diffusion or ducking. Stereo
follows `Delay`: independent ring and filter state per channel, same time on both. Triplet
divisions would be additional table entries after index 5 (existing indices must not move).

Tests: `native/test/dub_delay_test.cpp` (divisions, BPM, feedback, loop filter, mix, extremes,
stereo, glide), `test/paramIds.test.ts`, `native/test/smoke.mjs`.
