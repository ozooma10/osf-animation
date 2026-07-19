import { describe, expect, it } from "vitest";
import { encodeCommand, parseNativeMessage } from "../src/bridge";

describe("bridge contract", () => {
  it("encodes the stable ui.command envelope", () => {
    expect(JSON.parse(encodeCommand("osf.animation.stop", { handle: 42 }))).toEqual({
      type: "ui.command",
      payload: { command: "osf.animation.stop", handle: 42 },
    });
  });

  it("accepts native messages and rejects malformed envelopes", () => {
    expect(parseNativeMessage('{"type":"runtime.ready","payload":{"protocol":"1.0"}}')).toEqual({
      type: "runtime.ready",
      payload: { protocol: "1.0" },
    });
    expect(parseNativeMessage("not json")).toBeNull();
    expect(parseNativeMessage('{"payload":{}}')).toBeNull();
  });
});
