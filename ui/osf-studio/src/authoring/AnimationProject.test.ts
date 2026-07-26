import * as THREE from "three";
import { describe, expect, it } from "vitest";
import {
  animationProjectReducer,
  applyProjectFrame,
  buildAnimationClip,
  captureBonePose,
  createAnimationProject,
  type AnimationProjectState,
} from "./AnimationProject";

function setup() {
  const project = createAnimationProject({ id: "rig", name: "skeleton.rig", sha256: "abc" });
  const state: AnimationProjectState = { project, past: [], future: [] };
  const bone = new THREE.Bone();
  bone.name = "Spine";
  return { state, bone };
}

describe("AnimationProject", () => {
  it("stores, interpolates, and exports local transform keys", () => {
    const { state, bone } = setup();
    let next = animationProjectReducer(state, {
      type: "setKey", bone: bone.name, frame: 0, pose: captureBonePose(bone),
    });
    bone.position.y = 2;
    next = animationProjectReducer(next, {
      type: "setKey", bone: bone.name, frame: 30, pose: captureBonePose(bone),
    });
    bone.position.y = 0;
    applyProjectFrame(next.project!, new Map([[bone.name, bone]]), new Map(), 15);
    expect(bone.position.y).toBeCloseTo(1);
    const clip = buildAnimationClip(next.project!);
    expect(clip.duration).toBe(1);
    expect(clip.tracks).toHaveLength(3);
  });

  it("supports undo, redo, duplicate, move, and delete commands", () => {
    const { state, bone } = setup();
    let next = animationProjectReducer(state, {
      type: "setKey", bone: bone.name, frame: 0, pose: captureBonePose(bone),
    });
    next = animationProjectReducer(next, {
      type: "duplicateKey", bone: bone.name, fromFrame: 0, toFrame: 10,
    });
    expect(next.project!.tracks.Spine.map((key) => key.frame)).toEqual([0, 10]);
    next = animationProjectReducer(next, {
      type: "moveKey", bone: bone.name, fromFrame: 10, toFrame: 20,
    });
    expect(next.project!.tracks.Spine.map((key) => key.frame)).toEqual([0, 20]);
    next = animationProjectReducer(next, { type: "undo" });
    expect(next.project!.tracks.Spine.map((key) => key.frame)).toEqual([0, 10]);
    next = animationProjectReducer(next, { type: "redo" });
    next = animationProjectReducer(next, { type: "deleteKey", bone: bone.name, frame: 20 });
    expect(next.project!.tracks.Spine.map((key) => key.frame)).toEqual([0]);
  });

  it("trims out-of-range keys when duration becomes shorter", () => {
    const { state, bone } = setup();
    let next = animationProjectReducer(state, {
      type: "setKey", bone: bone.name, frame: 30, pose: captureBonePose(bone),
    });
    next = animationProjectReducer(next, { type: "setDuration", durationFrames: 15 });
    expect(next.project!.tracks.Spine).toBeUndefined();
  });
});
