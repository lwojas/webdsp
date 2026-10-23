import { describe, expect, it } from "vitest";
import { HOST_PROTOCOL_VERSION, hostProtocolWarning } from "../src/runtime/commandProtocol";

describe("host protocol version", () => {
  it("is silent for a browser (no version) and for a host at or above this runtime's version", () => {
    expect(hostProtocolWarning(undefined)).toBeNull();
    expect(hostProtocolWarning(HOST_PROTOCOL_VERSION)).toBeNull();
    expect(hostProtocolWarning(HOST_PROTOCOL_VERSION + 1)).toBeNull();
  });

  it("warns, naming both versions, for an older host", () => {
    const warning = hostProtocolWarning(HOST_PROTOCOL_VERSION - 1);
    expect(warning).toContain(`protocol ${HOST_PROTOCOL_VERSION - 1}`);
    expect(warning).toContain(`expects ${HOST_PROTOCOL_VERSION}`);
  });
});
