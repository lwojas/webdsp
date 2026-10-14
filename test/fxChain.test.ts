import { describe, expect, it } from "vitest";
import { AudioRuntime } from "../src/runtime";
import type { HostCommand } from "../src/runtime/commandProtocol";
import { EffectKind, MASTER_BUS, MAX_EFFECTS_PER_BUS, NodeParam } from "../src/runtime/types";

// AudioRuntime's constructor needs an AudioContext; the FX-chain surface only touches the
// bridge and its own bookkeeping, so build the instance without running the constructor.
function makeRuntime() {
  const sent: HostCommand[] = [];
  const rt = Object.create(AudioRuntime.prototype) as AudioRuntime;
  Object.assign(rt, {
    bridge: { send: (cmd: HostCommand) => sent.push(cmd) },
    effectChains: new Map(),
    nextEffectId: 1,
  });
  return { rt, sent };
}

describe("AudioRuntime FX chain", () => {
  it("allows several instances of one kind with distinct ids", () => {
    const { rt } = makeRuntime();
    const a = rt.addEffect(MASTER_BUS, EffectKind.Filter);
    const b = rt.addEffect(MASTER_BUS, EffectKind.Filter);
    expect(a).not.toBe(b);
    expect(rt.getEffectChain(MASTER_BUS).map((e) => e.kind)).toEqual([EffectKind.Filter, EffectKind.Filter]);
  });

  it("keeps chains per bus and honours insert index, move and remove", () => {
    const { rt, sent } = makeRuntime();
    const f = rt.addEffect(1, EffectKind.Filter);
    const d = rt.addEffect(1, EffectKind.Delay);
    const f2 = rt.addEffect(1, EffectKind.Filter, { index: 1 });
    expect(rt.getEffectChain(1).map((e) => e.id)).toEqual([f, f2, d]);
    expect(rt.getEffectChain(MASTER_BUS)).toEqual([]);
    rt.moveEffect(1, d, 0);
    expect(rt.getEffectChain(1).map((e) => e.id)).toEqual([d, f, f2]);
    rt.removeEffect(1, f);
    expect(rt.getEffectChain(1).map((e) => e.id)).toEqual([d, f2]);
    expect(sent.filter((c) => c.type === "remove-effect")).toHaveLength(1);
  });

  it("tracks enabled state and ignores unknown ids", () => {
    const { rt, sent } = makeRuntime();
    const f = rt.addEffect(0, EffectKind.Filter, { enabled: false });
    expect(rt.getEffectChain(0)[0].enabled).toBe(false);
    rt.setEffectEnabled(0, f, true);
    expect(rt.getEffectChain(0)[0].enabled).toBe(true);
    const before = sent.length;
    rt.setEffectEnabled(0, 999, false);
    rt.setEffectParameter(0, 999, NodeParam.FilterCutoff, 100);
    rt.removeEffect(0, 999);
    rt.moveEffect(0, 999, 0);
    expect(sent.length).toBe(before);
  });

  it("targets a specific instance with parameter writes and enforces the chain cap", () => {
    const { rt, sent } = makeRuntime();
    const a = rt.addEffect(0, EffectKind.Filter);
    const b = rt.addEffect(0, EffectKind.Filter);
    rt.setEffectParameter(0, b, NodeParam.FilterCutoff, 800);
    const cmd = sent[sent.length - 1];
    expect(cmd).toEqual({ type: "set-effect-param", bus: 0, effect: b, param: NodeParam.FilterCutoff, value: 800 });
    expect(a).not.toBe(b);
    for (let i = 2; i < MAX_EFFECTS_PER_BUS; i++) rt.addEffect(0, EffectKind.Delay);
    expect(() => rt.addEffect(0, EffectKind.Delay)).toThrow();
  });
});
