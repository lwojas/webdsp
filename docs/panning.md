# Stereo panning

Code: `native/src/dsp/panner.h` (the law and smoother), applied in `native/src/voice.h`
(`VoiceParam.Pan`, `TriggerParams.pan`) and `native/src/bus.h` (`NodeParam.BusPan`). Tests:
`native/test/pan_test.cpp`, plus a pan check in `native/test/smoke.mjs` (real WASM ABI).

## What it does

Positions a signal between the left and right output channels. The DSP layer knows nothing
about knobs or UI; a consumer (tracker, sequencer, MIDI mapper) just sets a number.

| | |
| --- | --- |
| Range | `-1` full left, `0` centre, `+1` full right. Out-of-range values are clamped; non-finite values are ignored (a trigger with a non-finite pan plays centred). |
| Default | `0` (centre). Centre is **bit-identical** to the engine's behaviour before pan existed, so existing projects do not change. |
| Smoothing | Gains are smoothed with a one-pole (~5 ms), so live changes do not click. A new note starts exactly at its pan (no glide-in from centre); a stolen/retriggered voice glides. At rest the smoother lands exactly on its target (hard pan is exactly silent on the far side). |

## Where it sits in the signal path

```
sample gain x voice gain x envelope -> voice filter
  -> VOICE PAN  (VoiceParam.Pan / TriggerParams.pan)       per note / event
  -> [sum into the voice's bus]
  -> bus chain -> BusGain -> BUS PAN (NodeParam.BusPan)     per track / master
  -> [track buses sum into master] -> master chain -> BusGain -> BUS PAN -> output
```

Two stages, one law implementation:

* **Voice pan** places an individual note (per-event pan, e.g. a tracker's pan column). It is
  applied after the voice filter and before the bus sum so it never modulates filter state.
* **Bus pan** is a channel-strip pan for a whole track (or the master): set once with
  `setNodeParameter(bus, NodeParam.BusPan, v)`. It sits after `BusGain`, the bus's output
  stage. It does not count as "configuring" a dormant bus (like gain: zero in, zero out).
`BusGain` itself glides across the block it changes in (`native/test/bus_gain_test.cpp`).

They compose: a voice panned left on a bus panned left is further left.

* **Instrument default pan** (`SampleParam.Pan`, or `loadSample({ pan })`) is a persistent
  property of the sample, like `SampleParam.Gain`. It is **added** to the voice's own pan
  (`TriggerParams.pan` / `VoiceParam.Pan`) and the sum is clamped to -1..+1, so a tracker can
  set an instrument's home position and let individual notes offset from it. It is read live:
  editing it moves voices already playing that sample (smoothed). Default 0; non-finite
  values are ignored. The law (mono vs stereo) follows the sample's channel count as above.

## Pan law

Which law is used depends on what is being placed.

**Mono source** (a 1-channel sample): constant-power, normalised so centre is unity.

```
theta = (pan + 1) * pi/4
gL = sqrt(2) * cos(theta)      gR = sqrt(2) * sin(theta)
```

* Centre: `gL = gR = 1` exactly. A mono sample is already duplicated unscaled onto both
  channels by `Voice::render`, so unity at centre is exactly the old output.
* `gL^2 + gR^2 = 2` everywhere: constant power across the sweep, no loudness bump when moving
  a sound around.
* Full left/right: far channel exactly `0`, near channel `sqrt(2)` (+3 dB vs centre per
  channel; total power unchanged).
* Why normalised: the common "-3 dB centre" form (`cos`/`sin` without `sqrt(2)`) would drop
  every existing centred sample by 3 dB, violating the requirement that defaults not change
  behaviour. The cost of the choice is that a hard-panned full-scale mono sample peaks at
  `sqrt(2)`; leave headroom (sample/voice/bus gain) as with any hot signal.

**Stereo source** (a 2-channel sample, or any bus): balance.

```
pan <= 0:  gL = 1,          gR = cos(-pan * pi/2)
pan >  0:  gL = cos(pan * pi/2),  gR = 1
```

* Centre is unity; full left silences R exactly; nothing is ever boosted.
* The channels are **not** cross-mixed: panning a stereo sample hard left drops its right
  channel rather than folding it into the left. This is the usual meaning of a pan knob on a
  stereo track (balance) and the least surprising option, since it never adds energy or
  collapses a stereo image.

Consequence for buses: a bus carries a stereo mix, and dual-mono content is indistinguishable
from stereo there, so `BusPan` is always balance. A mono sample panned hard left via voice pan
reads `sqrt(2)` on L; the same sample on a bus panned hard left reads `1` on L (-3 dB). If you
want mono-source loudness behaviour, pan per voice; use bus pan for track balance.

## Mono and stereo sources in this engine

Both exist. Output is stereo (`kMaxDspChannels = 2`). `Voice::render` reads each output channel
from the sample's own channel (`min(c, channels-1)`), so a mono sample is "dual mono" and a
stereo sample plays natively L/R. Panning uses `sample.channels == 1` to choose the law. There
is no 3+ channel support anywhere in the engine; surround would be a separate project.

## API

```ts
runtime.trigger({ sampleId, pan: -0.5 });                // per note
runtime.schedule([{ time, sampleId, pan: 0.25 }]);        // per scheduled event
runtime.setVoiceParameter(voice, VoiceParam.Pan, 0.8);    // live voice, smoothed
runtime.setNodeParameter(bus, NodeParam.BusPan, -1);      // track or MASTER_BUS
runtime.setSampleParameter(sample, SampleParam.Pan, 0.3); // instrument default, live
await runtime.loadSample(buf, { pan: -0.4 });              // or at load time
```

ABI change: `webdsp_trigger` and `webdsp_schedule_event` gained a trailing `float pan`
argument (`engine.h`/`scheduler.h` default it to 0, so native call sites without it still
compile). The WASM and `lib/` outputs were rebuilt.

## Limitations / next steps

* Stereo "width"/mid-side and true stereo pan (cross-mix pan law for stereo material) are not
  provided.
* Pan is not a `DSPNode` in the chain; it is a plain stage inside `Voice::render` and
  `Bus::process`, because it needs to know the source's channel count.
