import { afterEach, describe, expect, it } from "vitest";
import { getHostState, type HostStateBridge } from "../src/runtime/hostState";

const g = globalThis as { __webdspHost?: { kind: string; state?: HostStateBridge } };
afterEach(() => {
  delete g.__webdspHost;
});

describe("host state bridge", () => {
  it("is null in a plain browser/node, and when a host offers none", () => {
    expect(getHostState()).toBeNull();
    g.__webdspHost = { kind: "native" };
    expect(getHostState()).toBeNull();
  });

  it("is whatever the host installed", () => {
    const published: unknown[] = [];
    const bridge: HostStateBridge = { publish: (s) => published.push(s), clear: () => published.push("clear"), onRestore: () => () => {} };
    g.__webdspHost = { kind: "native", state: bridge };
    expect(getHostState()).toBe(bridge);
    getHostState()!.publish({ app: "webseq", format: 1, data: "{}", blobs: {} });
    getHostState()!.clear();
    expect(published).toEqual([{ app: "webseq", format: 1, data: "{}", blobs: {} }, "clear"]);
  });
});
