import * as THREE from "three";
import { describe, expect, it } from "vitest";
import { AnimationAuthoring, AUTHORING_FPS } from "./AnimationAuthoring";

describe("AnimationAuthoring", () => {
  it("quantizes keys to 30 FPS and interpolates local bone transforms", () => {
    const authoring = new AnimationAuthoring();
    const bone = new THREE.Bone();

    authoring.setKey(bone, 0);
    bone.position.set(0, 2, 0);
    bone.quaternion.setFromAxisAngle(new THREE.Vector3(0, 1, 0), Math.PI);
    authoring.setKey(bone, 1);

    bone.position.set(0, 0, 0);
    bone.quaternion.identity();
    authoring.apply([bone], 0.5);

    expect(AUTHORING_FPS).toBe(30);
    expect(authoring.keyTimes()).toEqual([0, 1]);
    expect(bone.position.y).toBeCloseTo(1);
    const expected = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 1, 0), Math.PI / 2);
    expect(bone.quaternion.angleTo(expected)).toBeLessThan(0.00001);
  });

  it("supports undo and redo for authored keys", () => {
    const authoring = new AnimationAuthoring();
    const bone = new THREE.Bone();

    authoring.setKey(bone, 0);
    bone.position.x = 4;
    authoring.setKey(bone, 1);

    expect(authoring.keyTimes()).toEqual([0, 1]);
    expect(authoring.undo()).toBe(true);
    expect(authoring.keyTimes()).toEqual([0]);
    expect(authoring.redo()).toBe(true);
    expect(authoring.keyTimes()).toEqual([0, 1]);
  });

  it("deletes only the selected bone key at the current frame", () => {
    const authoring = new AnimationAuthoring();
    const left = new THREE.Bone();
    const right = new THREE.Bone();

    authoring.setKey(left, 10 / 30);
    authoring.setKey(right, 10 / 30);
    expect(authoring.keyedBoneCount).toBe(2);
    expect(authoring.deleteKey(left, 10 / 30)).toBe(true);
    expect(authoring.keyedBoneCount).toBe(1);
    expect(authoring.hasKey(right, 10 / 30)).toBe(true);
  });
});
