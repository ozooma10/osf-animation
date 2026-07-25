import { describe, expect, it } from "vitest";
import {
  FILE_LIMITS,
  assertFileSize,
  validateEmbeddedGltf,
  validateGlbSignature,
} from "./fileSafety";

describe("file safety", () => {
  it("enforces centralized allocation limits", () => {
    expect(() => assertFileSize({ size: FILE_LIMITS.json + 1 } as File, "json")).toThrow(/5 MiB/);
    expect(() => assertFileSize({ size: FILE_LIMITS.rig } as File, "rig")).not.toThrow();
    expect(() => assertFileSize({ size: FILE_LIMITS.bundle + 1 } as File, "bundle")).toThrow(/256 MiB/);
  });

  it("validates GLB, gzip, and truncated signatures", () => {
    expect(validateGlbSignature(new Uint8Array([0x67, 0x6c, 0x54, 0x46]).buffer)).toBe("glb");
    expect(validateGlbSignature(new Uint8Array([0x1f, 0x8b, 0, 0]).buffer)).toBe("gzip");
    expect(() => validateGlbSignature(new Uint8Array([1]).buffer)).toThrow(/truncated/);
  });

  it("rejects external glTF dependencies with an actionable message", () => {
    expect(() => validateEmbeddedGltf(JSON.stringify({
      asset: { version: "2.0" },
      buffers: [{ uri: "model.bin" }],
    }))).toThrow(/Embed its buffers/);
    expect(() => validateEmbeddedGltf(JSON.stringify({
      asset: { version: "2.0" },
      buffers: [{ uri: "data:application/octet-stream;base64,AA==" }],
    }))).not.toThrow();
  });
});

