# Mixer scheduling: bus targets and fixed-pitch MIDI routes (protocol 4)

Two additive changes that let a sequencer put its mixer, not only its notes, on the audio clock, and let a plugin host play a drum pad
properly. Both need a **native host** that implements protocol 4 (`capabilities.hostProtocolVersion >= 4`); neither changes what a
browser does. The first client to use them is Pulse.8 (`PulseEight`, `src/engine/nativeseq.ts`), whose VST3 plays its whole song,
per-pattern mix included, on the audio thread.

## 1. Bus targets in scheduled params and native sequences

Until now a scheduled parameter event could address an effect, an instrument or a sample. A bus's own mixer parameters could only be
written immediately, so an application that sequenced from the page could not move a fader or a send at a musical time once the page
was not the clock (a plugin window closed, an offline bounce).

```ts
runtime.scheduleParams([
  { time, target: { bus: busId }, param: NodeParam.BusSendAmount, value: 0.4 },
  { time, target: { bus: busId }, param: NodeParam.BusGain, from: 1, value: 0, ramp: 0.5 },   // a fader ramp
]);

// and in a native Sequence, in beats:
{ beat: 8, kind: "param", target: { bus: busId }, param: NodeParam.BusMainGain, value: 0 }
```

| | |
|---|---|
| Target | `{ bus: BusId }` (`ScheduledParamTarget`; wire target code `3`; native `ParamTarget::Bus`). `MASTER_BUS` (0) and any bus `createBus()` handed out. |
| Parameters | The mixer's `NodeParam`s only: `BusGain`, `BusPan`, `BusSendAmount`, `BusMainGain`. Routing (`BusOutput`, `BusSendTarget`) is structure, not automation, and effect parameters keep their own target; both are dropped if sent through a bus. |
| Write path | `Engine::writeBusParam`, the path `setBusParam` uses, so a change **glides across its block** exactly as an immediate one does (no zipper on a fader or send). Sends and main gain exist on track buses only; on the master bus they are ignored. |
| Ramps | As for other targets: linear in the parameter's units, one ramp per (bus, param). A ramp with no `from` starts at the current base, which is readable for `BusGain`, `BusSendAmount` and `BusMainGain`; give a `BusPan` ramp a `from`. |
| Last writer wins | An immediate `setNodeParameter` on a bus mixer parameter now also ends that parameter's running scheduled ramp (`setBusParam` cancels it), as `setEffectParameter` does for effects. |
| Unknown | An unknown bus, a non-mixer parameter or a non-finite value is dropped. |
| Browser | **Not available.** The worklet's WASM predates the target, so `AudioRuntime.scheduleParams` and `setSequence` drop `{ bus }` events unless the host reports protocol >= 4 (`knowsBusTarget`). The WASM was not rebuilt for this; rebuild it (`npm run build:wasm`) before a browser app needs bus scheduling. Nothing in the browser path changed. |

Why it matters: a pattern's sound **and mix** (fader, mutes, delay and reverb sends, punch section sends) can now be baked into a native
`Sequence` and change on the audio clock at a pattern boundary or in the middle of a written FX lane. Without it, the plugin could only
play notes natively and the page would have to be open to move the mix.

Code: `native/src/param_events.h` (`ParamTarget::Bus`, count 4), `native/src/engine.h` (`applyParam`, `currentParam`,
`isMixerParam`, `hasBus`, `writeBusParam`), `native/src/bus.h` (`gainBase()`), `src/runtime/{types,index,sequence,commandProtocol}.ts`.
Test: `native/test/param_events_test.cpp` (`testBusTarget`: an audible send step, a ramp from the base, the fader and main gain, a direct
write ending a ramp, non-mixer params and unknown buses dropped), `test/modulation.test.ts`, `test/sequence.test.ts`.

## 2. `fixedPitch` on an instrument MIDI route

A MIDI route (`runtime.setMidiRoute`) plays a key as `note - unityNote` semitones from the target's pitch. Right for a keyboard playing a
synth; wrong for a drum pad, where every pad of a controller sends a different note and the player wants the same drum, not the drum
transposed by the pad's number.

```ts
runtime.setMidiRoute({ kind: "instrument", id: snare, unityNote: 60, fixedPitch: true });   // any key: the snare at its own pitch
```

`fixedPitch` (instruments only; ignored for samples) makes the host play every key at pitch 0. Velocity still scales the level, and
`holdUntilKeyUp`, `gain`, `chokeGroup` and `channel` apply as before. Code: `MidiRoute.fixedPitch` / `normalizeMidiRoute`
(`src/runtime/midiRoute.ts`); the host honours it (webvst `EngineHost`, `Route::fixedPitch`). Test: `test/midiRoute.test.ts`, and webvst's
host test (a fixed-pitch route plays every key identically, a normal one transposes).

## Protocol

Both ride **`HOST_PROTOCOL_VERSION` 4**. A host reporting less gets the usual console warning (`hostProtocolWarning`), not a failure:
apps that do not use these features run unchanged against an older plugin, and an app that does should feature-test
`capabilities.hostProtocolVersion >= 4` and fall back to sequencing from the page (Pulse.8 does). The wire shape is additive: target
code 3 in `schedule-params`, `{ bus }` in a `set-sequence` param event, `fixedPitch` in `set-midi-route`. See `docs/native-host.md`.

## Using it from a client

1. Ask whether the host can: `rt.getCapabilities().nativeSequence && (rt.getCapabilities().hostProtocolVersion ?? 0) >= 4`.
2. Bake state that **differs between patterns** into the sequence as `{ bus }` / effect / instrument param events at the pattern's start
   (and at every bar, so a locate into the middle of a pattern is repaired within a bar). State that is the same everywhere needs no
   events: the page's immediate writes set it.
3. Keep mixer moves the player makes live (a mute, a fader) as immediate writes **and** in the next sequence, so both agree.
4. A parameter the player is holding live (a punch) should not also be in the sequence: leave it out while it is held.
