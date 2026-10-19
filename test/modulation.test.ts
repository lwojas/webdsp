import { describe, expect, it } from "vitest";
import { AudioRuntime } from "../src/runtime";
import type { HostCommand } from "../src/runtime/commandProtocol";
import { EffectKind, InstrumentKind, LfoWaveform } from "../src/runtime/types";

const bassParams = [
  { id: 0, name: "tune", min: 27.5, max: 110, default: 55, unit: "Hz", modulation: "log" as const },
  { id: 3, name: "cutoff", min: 40, max: 4000, default: 400, unit: "Hz", modulation: "log" as const },
  { id: 5, name: "attack", min: 0.001, max: 0.25, default: 0.003, unit: "s", modulation: "none" as const },
];
const modulation = {
  maxLfos: 4,
  waveforms: ["sine", "triangle", "square", "saw"],
  divisions: [{ name: "1/1", beats: 4 }, { name: "1/4", beats: 1 }, { name: "1/8", beats: 0.5 }],
  minRateHz: 0.01,
  maxRateHz: 40,
};

function makeRuntime() {
  const sent: HostCommand[] = [];
  const rt = Object.create(AudioRuntime.prototype) as AudioRuntime;
  Object.assign(rt, {
    bridge: { send: (cmd: HostCommand) => sent.push(cmd) },
    capabilities: {
      instruments: [{ kind: InstrumentKind.Bass, id: "bass", name: "Bass", category: "bass", params: bassParams }],
      modulation,
    },
    instruments: new Map(),
    nextInstrumentId: 1,
  });
  return { rt, sent };
}

describe("AudioRuntime LFO modulation", () => {
  it("derives modulation targets from the synth's own parameter metadata", () => {
    const { rt } = makeRuntime();
    expect(rt.getModulationTargets(InstrumentKind.Bass).map((p) => p.name)).toEqual(["tune", "cutoff"]);
    expect(rt.getModulationTargets(InstrumentKind.Kick)).toEqual([]);
    expect(rt.getModulationInfo().maxLfos).toBe(4);
  });

  it("sends LFO settings with defaults (free, sine, 1 Hz, 1/4)", () => {
    const { rt, sent } = makeRuntime();
    const b = rt.createInstrument(InstrumentKind.Bass);
    sent.length = 0;
    rt.setInstrumentLfo(b, 1, { target: 3, amount: 0.35 });
    expect(sent).toEqual([{
      type: "set-instrument-lfo", instrument: b, slot: 1, enabled: true, waveform: LfoWaveform.Sine,
      sync: false, rateHz: 1, division: 1, phase: 0, amount: 0.35, target: 3, retrigger: false,
    }]);
    rt.setInstrumentLfo(b, 0, { target: 0, amount: -0.2, waveform: LfoWaveform.Saw, sync: true, division: 2, phase: 0.5 });
    expect(sent[1]).toMatchObject({ slot: 0, waveform: LfoWaveform.Saw, sync: true, division: 2, phase: 0.5, amount: -0.2 });
  });

  it("clears a slot and ignores bad slots or unknown instruments", () => {
    const { rt, sent } = makeRuntime();
    const b = rt.createInstrument(InstrumentKind.Bass);
    sent.length = 0;
    rt.clearInstrumentLfo(b, 2);
    expect(sent).toEqual([expect.objectContaining({ type: "set-instrument-lfo", slot: 2, enabled: false })]);
    sent.length = 0;
    rt.setInstrumentLfo(b, 4, { target: 3, amount: 1 });
    rt.setInstrumentLfo(b, -1, { target: 3, amount: 1 });
    rt.setInstrumentLfo(b, 0.5, { target: 3, amount: 1 });
    rt.setInstrumentLfo(999, 0, { target: 3, amount: 1 });
    rt.clearInstrumentLfo(999, 0);
    expect(sent).toEqual([]);
  });
});

describe("AudioRuntime effect LFO modulation", () => {
  const utilityParams = [{ id: 41, name: "gain", min: -60, max: 12, default: 0, unit: "dB", modulation: "linear" as const }];
  const filterParams = [
    { id: 6, name: "mode", min: 0, max: 1, default: 0, unit: "step", modulation: "none" as const },
    { id: 4, name: "cutoff", min: 40, max: 18000, default: 18000, unit: "Hz", modulation: "log" as const },
  ];
  function make() {
    const sent: HostCommand[] = [];
    const rt = Object.create(AudioRuntime.prototype) as AudioRuntime;
    Object.assign(rt, {
      bridge: { send: (cmd: HostCommand) => sent.push(cmd) },
      capabilities: {
        instruments: [],
        effects: [
          { kind: EffectKind.Filter, params: filterParams },
          { kind: EffectKind.Utility, params: utilityParams },
        ],
        modulation,
      },
      effectChains: new Map(),
      nextEffectId: 1,
    });
    const bus = 0;
    const fx = rt.addEffect(bus, EffectKind.Utility);
    sent.length = 0;
    return { rt, sent, bus, fx };
  }

  it("lists effect parameters and the modulation targets of a kind", () => {
    const { rt } = make();
    expect(rt.getEffectParams(EffectKind.Utility).map((p) => p.name)).toEqual(["gain"]);
    expect(rt.getEffectModulationTargets(EffectKind.Filter).map((p) => p.name)).toEqual(["cutoff"]);
    expect(rt.getEffectParams(EffectKind.Reverb)).toEqual([]);
  });

  it("sends effect LFO settings with the same defaults as an instrument LFO", () => {
    const { rt, sent, bus, fx } = make();
    rt.setEffectLfo(bus, fx, 1, { target: 41, amount: 0.3 });
    expect(sent).toEqual([{
      type: "set-effect-lfo", bus, effect: fx, slot: 1, enabled: true, waveform: LfoWaveform.Sine,
      sync: false, rateHz: 1, division: 1, phase: 0, amount: 0.3, target: 41,
    }]);
    rt.clearEffectLfo(bus, fx, 1);
    expect(sent[1]).toMatchObject({ type: "set-effect-lfo", slot: 1, enabled: false });
  });

  it("ignores bad slots and unknown effects", () => {
    const { rt, sent, bus, fx } = make();
    rt.setEffectLfo(bus, fx, 4, { target: 41, amount: 1 });
    rt.setEffectLfo(bus, fx, -1, { target: 41, amount: 1 });
    rt.setEffectLfo(bus, 999, 0, { target: 41, amount: 1 });
    rt.clearEffectLfo(bus, 999, 0);
    expect(sent).toEqual([]);
  });
});

describe("AudioRuntime scheduled parameters", () => {
  function make() {
    const sent: HostCommand[] = [];
    const rt = Object.create(AudioRuntime.prototype) as AudioRuntime;
    Object.assign(rt, {
      bridge: { send: (cmd: HostCommand) => sent.push(cmd) },
      capabilities: { instruments: [], effects: [], modulation },
      effectChains: new Map(),
      nextEffectId: 1,
      instruments: new Map([[5, InstrumentKind.Bass]]),
      samples: new Map([[9, {}]]),
    });
    const fx = rt.addEffect(0, EffectKind.Utility);
    sent.length = 0;
    return { rt, sent, fx };
  }

  it("sends effect, instrument and sample events as one batch with the engine's target codes", () => {
    const { rt, sent, fx } = make();
    rt.scheduleParams([
      { time: 1, target: { effect: { bus: 0, effect: fx } }, param: 41, value: -6, from: 0, ramp: 0.5 },
      { time: 2, target: { instrument: 5 }, param: 3, value: 800 },
      { time: 3, target: { sample: 9 }, param: 0, value: 0.5 },
    ]);
    expect(sent).toEqual([{
      type: "schedule-params",
      events: [
        { time: 1, target: 0, bus: 0, id: fx, param: 41, value: -6, from: 0, ramp: 0.5 },
        { time: 2, target: 1, bus: 0, id: 5, param: 3, value: 800, from: undefined, ramp: 0 },
        { time: 3, target: 2, bus: 0, id: 9, param: 0, value: 0.5, from: undefined, ramp: 0 },
      ],
    }]);
  });

  it("drops unknown targets and non-finite or negative numbers, sending nothing if none remain", () => {
    const { rt, sent, fx } = make();
    rt.scheduleParams([
      { time: 1, target: { effect: { bus: 0, effect: 999 } }, param: 41, value: 0 },
      { time: 1, target: { effect: { bus: 3, effect: fx } }, param: 41, value: 0 },
      { time: 1, target: { instrument: 77 }, param: 3, value: 1 },
      { time: 1, target: { sample: 77 }, param: 0, value: 1 },
      { time: NaN, target: { instrument: 5 }, param: 3, value: 1 },
      { time: 1, target: { instrument: 5 }, param: 3, value: Infinity },
      { time: 1, target: { instrument: 5 }, param: 3, value: 1, ramp: -1 },
      { time: 1, target: { instrument: 5 }, param: 3, value: 1, from: NaN },
    ]);
    expect(sent).toEqual([]);
  });
});
