import { afterEach, describe, expect, it } from "vitest";
import { getHostFiles, type HostFilesBridge } from "../src/runtime/hostFiles";

const g = globalThis as { __webdspHost?: { kind: string; files?: HostFilesBridge } };
afterEach(() => {
  delete g.__webdspHost;
});

describe("host files bridge", () => {
  it("is null in a plain browser/node, and when a host offers none", () => {
    expect(getHostFiles()).toBeNull();
    g.__webdspHost = { kind: "native" };
    expect(getHostFiles()).toBeNull();
  });

  it("is whatever the host installed", async () => {
    const bridge: HostFilesBridge = {
      saveFile: async (o) => ({ name: o.suggestedName }),
      openFile: async () => ({ name: "a.json", data: new ArrayBuffer(2) }),
    };
    g.__webdspHost = { kind: "native", files: bridge };
    expect(getHostFiles()).toBe(bridge);
    expect(await getHostFiles()!.saveFile({ suggestedName: "x.json", data: "{}" })).toEqual({ name: "x.json" });
    expect((await getHostFiles()!.openFile({ extensions: ["json"] }))?.name).toBe("a.json");
  });
});
