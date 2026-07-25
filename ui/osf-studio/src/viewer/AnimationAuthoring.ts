import * as THREE from "three";

export const AUTHORING_FPS = 30;

interface BonePose {
  position: [number, number, number];
  quaternion: [number, number, number, number];
  scale: [number, number, number];
}

interface BoneKey {
  frame: number;
  pose: BonePose;
}

type TrackMap = Map<string, BoneKey[]>;

function capturePose(bone: THREE.Bone): BonePose {
  return {
    position: bone.position.toArray(),
    quaternion: bone.quaternion.toArray(),
    scale: bone.scale.toArray(),
  };
}

function cloneTracks(source: TrackMap): TrackMap {
  return new Map([...source].map(([id, keys]) => [
    id,
    keys.map((key) => ({
      frame: key.frame,
      pose: {
        position: [...key.pose.position],
        quaternion: [...key.pose.quaternion],
        scale: [...key.pose.scale],
      },
    })),
  ]));
}

export class AnimationAuthoring {
  private tracks: TrackMap = new Map();
  private undoStack: TrackMap[] = [];
  private redoStack: TrackMap[] = [];

  get dirty(): boolean {
    return this.tracks.size > 0;
  }

  get canUndo(): boolean {
    return this.undoStack.length > 0;
  }

  get canRedo(): boolean {
    return this.redoStack.length > 0;
  }

  get keyedBoneCount(): number {
    return this.tracks.size;
  }

  frameAt(time: number): number {
    return Math.max(0, Math.round(time * AUTHORING_FPS));
  }

  keyTimes(): number[] {
    return [...new Set(
      [...this.tracks.values()].flatMap((keys) => keys.map((key) => key.frame / AUTHORING_FPS)),
    )].sort((left, right) => left - right);
  }

  hasKey(bone: THREE.Bone | undefined, time: number): boolean {
    if (!bone) return false;
    const frame = this.frameAt(time);
    return this.tracks.get(bone.uuid)?.some((key) => key.frame === frame) ?? false;
  }

  setKey(bone: THREE.Bone, time: number): void {
    this.remember();
    const frame = this.frameAt(time);
    const keys = [...(this.tracks.get(bone.uuid) ?? [])];
    const existing = keys.findIndex((key) => key.frame === frame);
    const next = { frame, pose: capturePose(bone) };
    if (existing >= 0) keys[existing] = next;
    else keys.push(next);
    keys.sort((left, right) => left.frame - right.frame);
    this.tracks.set(bone.uuid, keys);
  }

  deleteKey(bone: THREE.Bone, time: number): boolean {
    const frame = this.frameAt(time);
    const keys = this.tracks.get(bone.uuid);
    if (!keys?.some((key) => key.frame === frame)) return false;
    this.remember();
    const remaining = keys.filter((key) => key.frame !== frame);
    if (remaining.length) this.tracks.set(bone.uuid, remaining);
    else this.tracks.delete(bone.uuid);
    return true;
  }

  apply(bones: THREE.Bone[], time: number): void {
    const frame = Math.max(0, time * AUTHORING_FPS);
    for (const bone of bones) {
      const keys = this.tracks.get(bone.uuid);
      if (!keys?.length) continue;
      const [left, right, alpha] = this.sample(keys, frame);
      bone.position.fromArray(left.pose.position).lerp(
        new THREE.Vector3().fromArray(right.pose.position),
        alpha,
      );
      bone.quaternion.fromArray(left.pose.quaternion).slerp(
        new THREE.Quaternion().fromArray(right.pose.quaternion),
        alpha,
      ).normalize();
      bone.scale.fromArray(left.pose.scale).lerp(
        new THREE.Vector3().fromArray(right.pose.scale),
        alpha,
      );
    }
  }

  undo(): boolean {
    const previous = this.undoStack.pop();
    if (!previous) return false;
    this.redoStack.push(cloneTracks(this.tracks));
    this.tracks = previous;
    return true;
  }

  redo(): boolean {
    const next = this.redoStack.pop();
    if (!next) return false;
    this.undoStack.push(cloneTracks(this.tracks));
    this.tracks = next;
    return true;
  }

  private remember(): void {
    this.undoStack.push(cloneTracks(this.tracks));
    if (this.undoStack.length > 100) this.undoStack.shift();
    this.redoStack = [];
  }

  private sample(keys: BoneKey[], frame: number): [BoneKey, BoneKey, number] {
    if (keys.length === 1 || frame <= keys[0].frame) return [keys[0], keys[0], 0];
    const last = keys[keys.length - 1];
    if (frame >= last.frame) return [last, last, 0];
    const rightIndex = keys.findIndex((key) => key.frame >= frame);
    const left = keys[rightIndex - 1];
    const right = keys[rightIndex];
    return [left, right, (frame - left.frame) / (right.frame - left.frame)];
  }
}
