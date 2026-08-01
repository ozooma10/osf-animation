import { describe, expect, it } from "vitest";
import { isRecord } from "../src/bridge/contract";

describe("bridge contract", () => {
  it("accepts object payloads and rejects non-record bridge values", () => {
    expect(isRecord({ protocol: "2.0" })).toBe(true);
    expect(isRecord([])).toBe(false);
    expect(isRecord(null)).toBe(false);
  });
});
