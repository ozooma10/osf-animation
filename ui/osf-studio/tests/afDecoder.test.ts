import { describe, expect, it } from "vitest";
import { decodeAf } from "../src/afDecoder";

function makeRig() {
  const buffer = new ArrayBuffer(288);
  const view = new DataView(buffer);
  view.setFloat32(48, 0.03125, true);
  view.setFloat32(52, 0.00025, true);
  view.setUint16(56, 2, true);
  view.setUint16(58, 1, true);

  const writeBone = (offset: number, nameOffset: number, parent: number, z: number) => {
    view.setFloat32(offset, 1, true);
    view.setFloat32(offset + 40, z, true);
    view.setBigUint64(offset + 48, BigInt(nameOffset), true);
    view.setInt32(offset + 56, parent, true);
  };
  writeBone(80, 272, -1, 0);
  writeBone(176, 277, 0, 1);
  new Uint8Array(buffer).set(new TextEncoder().encode("Root\0Child\0"), 272);
  return buffer;
}

function makeAf() {
  const buffer = new ArrayBuffer(66);
  const view = new DataView(buffer);
  view.setUint16(42, 1, true);
  view.setUint16(44, 30, true);
  view.setUint16(46, 2, true);
  new Uint8Array(buffer).set([1, 0], 64);
  return buffer;
}

describe("AF decoder", () => {
  it("builds a playable rig from an AF clip and skeleton.rig", () => {
    const decoded = decodeAf(makeAf(), makeRig(), "idle.af");
    expect(decoded.boneCount).toBe(2);
    expect(decoded.frameCount).toBe(30);
    expect(decoded.animations[0].name).toBe("idle");
    expect(decoded.animations[0].duration).toBeCloseTo(29 / 30);
    expect(decoded.scene.children[0].children).toHaveLength(1);
  });
});
