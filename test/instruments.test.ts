import { describe, expect, it } from "vitest";
import { AudioRuntime } from "../src/runtime";
import type { HostCommand } from "../src/runtime/commandProtocol";
import { InstrumentKind, KickParam, MASTER_BUS, MAX_INSTRUMENTS } from "../src/runtime/types";

const kickParams = [{ id: 0, name: "tune", min: 30, max: 120, default: 50, unit: "Hz" }];

function makeRuntime() {
  const sent: HostCommand[] = [];
  const rt = Object.create(AudioRuntime.prototype) as AudioRuntime;
  Object.assign(rt, {
    bridge: { send: (cmd: HostCommand) => sent.push(cmd) },
    capabilities: { instruments: [{ kind: InstrumentKind.Kick, params: kickParams }] },
    instruments: new Map(),
    nextInstrumentId: 1,
  });
  return { rt, sent };
}

describe("AudioRuntime instruments", () => {
  it("creates instances with distinct ids, routed to the requested bus", () => {
    const { rt, sent } = makeRuntime();
    const a = rt.createInstrument(InstrumentKind.Kick);
    const b = rt.createInstrument(InstrumentKind.Kick, { bus: 3 });
    expect(a).not.toBe(b);
    expect(sent).toEqual([
      { type: "create-instrument", instrument: a, kind: InstrumentKind.Kick, bus: MASTER_BUS },
      { type: "create-instrument", instrument: b, kind: InstrumentKind.Kick, bus: 3 },
    ]);
  });

  it("exposes engine-reported parameter metadata", () => {
    const { rt } = makeRuntime();
    expect(rt.getInstrumentParams(InstrumentKind.Kick)).toEqual(kickParams);
    expect(rt.getInstrumentParams(99 as InstrumentKind)).toEqual([]);
  });

  it("forwards notes and params, and ignores removed/unknown ids", () => {
    const { rt, sent } = makeRuntime();
    const k = rt.createInstrument(InstrumentKind.Kick);
    sent.length = 0;
    rt.setInstrumentParameter(k, KickParam.Decay, 0.8);
    rt.instrumentNoteOn(k, { velocity: 0.5, time: 2 });
    rt.instrumentNoteOff(k);
    expect(sent.map((c) => c.type)).toEqual(["set-instrument-param", "instrument-note-on", "instrument-note-off"]);
    rt.removeInstrument(k);
    sent.length = 0;
    rt.instrumentNoteOn(k);
    rt.setInstrumentParameter(k, KickParam.Tune, 60);
    rt.instrumentNoteOn(999);
    rt.removeInstrument(k);
    expect(sent).toEqual([]);
  });

  it("rejects unknown kinds and enforces MAX_INSTRUMENTS", () => {
    const { rt } = makeRuntime();
    expect(() => rt.createInstrument(42 as InstrumentKind)).toThrow();
    for (let i = 0; i < MAX_INSTRUMENTS; i++) rt.createInstrument(InstrumentKind.Kick);
    expect(() => rt.createInstrument(InstrumentKind.Kick)).toThrow();
  });
});
