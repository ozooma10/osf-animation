import * as THREE from "three";
import { describe, expect, it } from "vitest";
import { AnimatedNodeRig } from "../src/AnimationViewer";

describe("AnimatedNodeRig", () => {
  it("renders animated transform nodes when a GLB has no Bone objects", () => {
    const root = new THREE.Group();
    const hips = new THREE.Group();
    hips.name = "Hips";
    const spine = new THREE.Group();
    spine.name = "Spine";
    root.add(hips);
    hips.add(spine);

    const clip = new THREE.AnimationClip("pose", 1, [
      new THREE.VectorKeyframeTrack("Hips.position", [0, 1], [0, 0, 0, 0, 1, 0]),
      new THREE.QuaternionKeyframeTrack("Spine.quaternion", [0, 1], [0, 0, 0, 1, 0, 0, 0, 1]),
    ]);
    const rig = new AnimatedNodeRig(root, [clip]);

    expect(rig.nodeCount).toBe(2);
    expect(rig.group.children).toHaveLength(2);
    rig.dispose();
  });
});
