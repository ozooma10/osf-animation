import { describe, expect, it } from "vitest";
import { isRecord } from "../src/bridge/contract";
import { JsonTransferDecoder, JSON_TRANSFER_LIMITS } from "../src/bridge/transfer";

function parts(text = '{"scenes":[{"title":"é😀"}]}', id = "1") {
  const data = [text.slice(0, 11), text.slice(11)];
  return data.map((part, index) => ({ osfAnimationTransfer: 1, id, index, count: data.length,
    bytes: new TextEncoder().encode(text).length, data: part }));
}

describe("bridge contract", () => {
  it("accepts object payloads and rejects non-record bridge values", () => {
    expect(isRecord({ protocol: "2.0" })).toBe(true);
    expect(isRecord([])).toBe(false);
    expect(isRecord(null)).toBe(false);
  });
});

describe("bounded native JSON transfers", () => {
  it("delivers ordinary payloads without changing their released shape", () => {
    const payload = { scenes: [] };
    expect(new JsonTransferDecoder().consume("library", payload)).toEqual({ kind: "message", payload });
  });

  it("reassembles out-of-order UTF-8 chunks exactly once, tolerating identical duplicates", () => {
    const decoder = new JsonTransferDecoder();
    const chunks = parts();
    expect(decoder.consume("library", chunks[1])).toEqual({ kind: "pending" });
    expect(decoder.consume("library", chunks[1])).toEqual({ kind: "pending" });
    expect(decoder.consume("library", chunks[0])).toEqual({ kind: "message", payload: { scenes: [{ title: "é😀" }] } });
    for (const chunk of chunks) expect(decoder.consume("library", chunk)).toEqual({ kind: "pending" });
  });

  it("keeps concurrent event streams independent", () => {
    const decoder = new JsonTransferDecoder();
    const catalog = parts('{"scenes":["catalog"]}', "2");
    const library = parts('{"scenes":["library"]}', "3");
    expect(decoder.consume("catalog", catalog[0]).kind).toBe("pending");
    expect(decoder.consume("library", library[1]).kind).toBe("pending");
    expect(decoder.consume("catalog", catalog[1])).toEqual({ kind: "message", payload: { scenes: ["catalog"] } });
    expect(decoder.consume("library", library[0])).toEqual({ kind: "message", payload: { scenes: ["library"] } });
  });

  it("replaces an older transfer without mixing interleaved chunks or imprecise numeric IDs", () => {
    const decoder = new JsonTransferDecoder();
    const old = parts('{"scenes":["old"]}', "9007199254740992");
    const fresh = parts('{"scenes":["new"]}', "9007199254740993");
    decoder.consume("library", old[0]);
    decoder.consume("library", fresh[1]);
    expect(decoder.consume("library", old[1]).kind).toBe("pending");
    expect(decoder.consume("library", fresh[0])).toEqual({ kind: "message", payload: { scenes: ["new"] } });
    expect(decoder.consume("library", old[0]).kind).toBe("pending");
  });

  it("compares decimal IDs across digit boundaries", () => {
    const decoder = new JsonTransferDecoder();
    const old = parts(undefined, "9"), fresh = parts(undefined, "10");
    decoder.consume("library", old[0]);
    decoder.consume("library", fresh[0]);
    expect(decoder.consume("library", old[1]).kind).toBe("pending");
    expect(decoder.consume("library", fresh[1]).kind).toBe("message");
  });

  it("rejects a conflicting duplicate and allows a fresh transfer to recover", () => {
    const decoder = new JsonTransferDecoder(), chunks = parts();
    decoder.consume("library", chunks[0]);
    expect(decoder.consume("library", { ...chunks[0], data: "corrupt" }).kind).toBe("error");
    expect(decoder.consume("library", chunks[1]).kind).toBe("pending");
    const fresh = parts(undefined, "2");
    decoder.consume("library", fresh[0]);
    expect(decoder.consume("library", fresh[1]).kind).toBe("message");
    expect(decoder.consume("library", fresh[0]).kind).toBe("pending");
  });

  it.each([
    ["version", { osfAnimationTransfer: 2 }], ["zero ID", { id: "0" }], ["leading zero", { id: "01" }],
    ["numeric ID", { id: 1 }], ["overflow ID", { id: "18446744073709551616" }], ["negative index", { index: -1 }],
    ["fractional index", { index: 0.5 }], ["out-of-range index", { index: 2 }], ["empty count", { count: 0 }],
    ["huge count", { count: 1025 }], ["fractional count", { count: 2.5 }], ["zero bytes", { bytes: 0 }],
    ["huge document", { bytes: JSON_TRANSFER_LIMITS.bytes + 1 }], ["fractional bytes", { bytes: 25.5 }],
    ["impossible size", { bytes: 3 * JSON_TRANSFER_LIMITS.chunkBytes }], ["non-string data", { data: {} }],
    ["empty chunk", { data: "" }], ["huge chunk", { data: "x".repeat(JSON_TRANSFER_LIMITS.chunkBytes + 1) }],
    ["oversized UTF-8 chunk", { count: 2, bytes: 2 * JSON_TRANSFER_LIMITS.chunkBytes, data: "é".repeat(JSON_TRANSFER_LIMITS.chunkBytes) }],
  ])("rejects %s before exposing partial application data", (_name, overrides) => {
    expect(new JsonTransferDecoder().consume("library", { ...parts()[0], ...overrides }).kind).toBe("error");
  });

  it("rejects changed metadata, incorrect byte counts, and invalid assembled JSON", () => {
    const changed = new JsonTransferDecoder(), chunks = parts();
    changed.consume("library", chunks[0]);
    expect(changed.consume("library", { ...chunks[1], count: 3 }).kind).toBe("error");
    for (const delta of [-1, 1]) {
      const decoder = new JsonTransferDecoder();
      const incorrect = chunks.map((chunk) => ({ ...chunk, bytes: chunk.bytes + delta }));
      decoder.consume("library", incorrect[0]);
      expect(decoder.consume("library", incorrect[1]).kind).toBe("error");
    }
    const invalid = new JsonTransferDecoder(), bad = parts('{"scenes":["broken"}');
    invalid.consume("library", bad[0]);
    expect(invalid.consume("library", bad[1]).kind).toBe("error");
  });

  it("discards canceled chunks on hide/reset and on a newer ordinary snapshot", () => {
    const decoder = new JsonTransferDecoder(), chunks = parts();
    decoder.consume("library", chunks[0]); decoder.reset();
    for (const chunk of chunks) expect(decoder.consume("library", chunk).kind).toBe("pending");
    const newer = parts(undefined, "2"); decoder.consume("library", newer[0]);
    expect(decoder.consume("library", { scenes: [] })).toEqual({ kind: "message", payload: { scenes: [] } });
    expect(decoder.consume("library", newer[1]).kind).toBe("pending");
  });

  it("expires incomplete data without extending its lifetime on retries", () => {
    let now = 0;
    const decoder = new JsonTransferDecoder(() => now), chunks = parts();
    decoder.consume("library", chunks[0]); now = 29_999;
    decoder.consume("library", chunks[0]); now = 30_000;
    expect(decoder.consume("library", chunks[1]).kind).toBe("pending");
    const fresh = parts(undefined, "2"); decoder.consume("library", fresh[0]);
    expect(decoder.consume("library", fresh[1]).kind).toBe("message");
  });

  it("bounds concurrent streams and releases reservations on reset", () => {
    const decoder = new JsonTransferDecoder();
    for (let i = 0; i < JSON_TRANSFER_LIMITS.active; i++) expect(decoder.consume(`stream${i}`, parts()[0]).kind).toBe("pending");
    expect(decoder.consume("excess", parts()[0]).kind).toBe("error");
    decoder.reset();
    const fresh = parts(undefined, "2"); decoder.consume("excess", fresh[0]);
    expect(decoder.consume("excess", fresh[1]).kind).toBe("message");
  });

  it("bounds aggregate document reservations before buffering their contents", () => {
    const decoder = new JsonTransferDecoder();
    const chunk = { ...parts()[0], bytes: JSON_TRANSFER_LIMITS.bytes, count: 1024 };
    expect(decoder.consume("first", chunk).kind).toBe("pending");
    expect(decoder.consume("second", chunk).kind).toBe("pending");
    expect(decoder.consume("third", chunk).kind).toBe("error");
    decoder.consume("first", {});
    expect(decoder.consume("third", { ...chunk, id: "2" }).kind).toBe("pending");
  });
});
