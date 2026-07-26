import * as THREE from "three";

export const ANIMATION_PROJECT_SCHEMA_VERSION = 1;
export const DEFAULT_AUTHORING_FPS = 30;
export const MAX_UNDO_STATES = 50;

export interface BonePose {
  position: [number, number, number];
  quaternion: [number, number, number, number];
  scale: [number, number, number];
}

export interface BoneKeyframe {
  frame: number;
  pose: BonePose;
}

export interface AnimationProject {
  schemaVersion: typeof ANIMATION_PROJECT_SCHEMA_VERSION;
  id: string;
  title: string;
  rigAssetId: string;
  rigName: string;
  rigSha256: string;
  fps: number;
  durationFrames: number;
  tracks: Record<string, BoneKeyframe[]>;
  createdAt: string;
  updatedAt: string;
}

export interface AnimationProjectState {
  project: AnimationProject | null;
  past: AnimationProject[];
  future: AnimationProject[];
}

export type AnimationProjectCommand =
  | { type: "load"; project: AnimationProject | null }
  | { type: "rename"; title: string }
  | { type: "setFps"; fps: number }
  | { type: "setDuration"; durationFrames: number }
  | { type: "setKey"; bone: string; frame: number; pose: BonePose }
  | { type: "deleteKey"; bone: string; frame: number }
  | { type: "moveKey"; bone: string; fromFrame: number; toFrame: number }
  | { type: "duplicateKey"; bone: string; fromFrame: number; toFrame: number }
  | { type: "undo" }
  | { type: "redo" };

export function createAnimationProject(
  rig: { id: string; name: string; sha256: string },
  title = "New Animation",
): AnimationProject {
  const now = new Date().toISOString();
  return {
    schemaVersion: ANIMATION_PROJECT_SCHEMA_VERSION,
    id: `animation-project:${crypto.randomUUID()}`,
    title,
    rigAssetId: rig.id,
    rigName: rig.name,
    rigSha256: rig.sha256,
    fps: DEFAULT_AUTHORING_FPS,
    durationFrames: DEFAULT_AUTHORING_FPS,
    tracks: {},
    createdAt: now,
    updatedAt: now,
  };
}

export function captureBonePose(bone: THREE.Object3D): BonePose {
  return {
    position: bone.position.toArray(),
    quaternion: bone.quaternion.toArray(),
    scale: bone.scale.toArray(),
  };
}

export function applyBonePose(bone: THREE.Object3D, pose: BonePose): void {
  bone.position.fromArray(pose.position);
  bone.quaternion.fromArray(pose.quaternion).normalize();
  bone.scale.fromArray(pose.scale);
}

function cloneProject(project: AnimationProject): AnimationProject {
  return structuredClone(project);
}

function updateProject(project: AnimationProject, update: (next: AnimationProject) => void): AnimationProject {
  const next = cloneProject(project);
  update(next);
  next.updatedAt = new Date().toISOString();
  return next;
}

function commit(state: AnimationProjectState, project: AnimationProject): AnimationProjectState {
  if (!state.project) return state;
  return {
    project,
    past: [...state.past, cloneProject(state.project)].slice(-MAX_UNDO_STATES),
    future: [],
  };
}

function normalizedFrame(frame: number, duration: number): number {
  return Math.max(0, Math.min(Math.round(frame), duration));
}

function putKey(project: AnimationProject, bone: string, frame: number, pose: BonePose): void {
  const target = normalizedFrame(frame, project.durationFrames);
  const keys = [...(project.tracks[bone] ?? [])];
  const existing = keys.findIndex((key) => key.frame === target);
  const next = { frame: target, pose: structuredClone(pose) };
  if (existing >= 0) keys[existing] = next;
  else keys.push(next);
  project.tracks[bone] = keys.sort((left, right) => left.frame - right.frame);
}

export function animationProjectReducer(
  state: AnimationProjectState,
  command: AnimationProjectCommand,
): AnimationProjectState {
  if (command.type === "load") return { project: command.project, past: [], future: [] };
  if (command.type === "undo") {
    const previous = state.past.at(-1);
    if (!previous || !state.project) return state;
    return {
      project: cloneProject(previous),
      past: state.past.slice(0, -1),
      future: [cloneProject(state.project), ...state.future].slice(0, MAX_UNDO_STATES),
    };
  }
  if (command.type === "redo") {
    const next = state.future[0];
    if (!next || !state.project) return state;
    return {
      project: cloneProject(next),
      past: [...state.past, cloneProject(state.project)].slice(-MAX_UNDO_STATES),
      future: state.future.slice(1),
    };
  }
  if (!state.project) return state;

  if (command.type === "rename") {
    return commit(state, updateProject(state.project, (project) => {
      project.title = command.title;
    }));
  }
  if (command.type === "setFps") {
    return commit(state, updateProject(state.project, (project) => {
      project.fps = [24, 30, 60].includes(command.fps) ? command.fps : DEFAULT_AUTHORING_FPS;
    }));
  }
  if (command.type === "setDuration") {
    return commit(state, updateProject(state.project, (project) => {
      project.durationFrames = Math.max(1, Math.round(command.durationFrames));
      Object.keys(project.tracks).forEach((bone) => {
        project.tracks[bone] = project.tracks[bone].filter((key) => key.frame <= project.durationFrames);
        if (!project.tracks[bone].length) delete project.tracks[bone];
      });
    }));
  }
  if (command.type === "setKey") {
    return commit(state, updateProject(state.project, (project) => {
      putKey(project, command.bone, command.frame, command.pose);
    }));
  }
  if (command.type === "deleteKey") {
    if (!state.project.tracks[command.bone]?.some((key) => key.frame === command.frame)) return state;
    return commit(state, updateProject(state.project, (project) => {
      project.tracks[command.bone] = project.tracks[command.bone]
        .filter((key) => key.frame !== command.frame);
      if (!project.tracks[command.bone].length) delete project.tracks[command.bone];
    }));
  }
  if (command.type === "moveKey" || command.type === "duplicateKey") {
    const source = state.project.tracks[command.bone]?.find((key) => key.frame === command.fromFrame);
    if (!source) return state;
    return commit(state, updateProject(state.project, (project) => {
      if (command.type === "moveKey") {
        project.tracks[command.bone] = project.tracks[command.bone]
          .filter((key) => key.frame !== command.fromFrame);
      }
      putKey(project, command.bone, command.toFrame, source.pose);
    }));
  }
  return state;
}

function sample(keys: BoneKeyframe[], frame: number): BonePose {
  if (keys.length === 1 || frame <= keys[0].frame) return structuredClone(keys[0].pose);
  const last = keys.at(-1)!;
  if (frame >= last.frame) return structuredClone(last.pose);
  const rightIndex = keys.findIndex((key) => key.frame >= frame);
  const left = keys[rightIndex - 1];
  const right = keys[rightIndex];
  const alpha = (frame - left.frame) / (right.frame - left.frame);
  const position = new THREE.Vector3().fromArray(left.pose.position)
    .lerp(new THREE.Vector3().fromArray(right.pose.position), alpha);
  const quaternion = new THREE.Quaternion().fromArray(left.pose.quaternion)
    .slerp(new THREE.Quaternion().fromArray(right.pose.quaternion), alpha).normalize();
  const scale = new THREE.Vector3().fromArray(left.pose.scale)
    .lerp(new THREE.Vector3().fromArray(right.pose.scale), alpha);
  return {
    position: position.toArray(),
    quaternion: quaternion.toArray(),
    scale: scale.toArray(),
  };
}

export function applyProjectFrame(
  project: AnimationProject,
  bones: Map<string, THREE.Bone>,
  bindPoses: Map<string, BonePose>,
  frame: number,
): void {
  bones.forEach((bone, name) => {
    const keys = project.tracks[name];
    const pose = keys?.length ? sample(keys, frame) : bindPoses.get(name);
    if (pose) applyBonePose(bone, pose);
  });
}

export function buildAnimationClip(project: AnimationProject, name = project.title): THREE.AnimationClip {
  const tracks: THREE.KeyframeTrack[] = [];
  Object.entries(project.tracks).forEach(([bone, keys]) => {
    if (!keys.length) return;
    const times = keys.map((key) => key.frame / project.fps);
    tracks.push(new THREE.VectorKeyframeTrack(
      `${bone}.position`,
      times,
      keys.flatMap((key) => key.pose.position),
    ));
    tracks.push(new THREE.QuaternionKeyframeTrack(
      `${bone}.quaternion`,
      times,
      keys.flatMap((key) => key.pose.quaternion),
    ));
    tracks.push(new THREE.VectorKeyframeTrack(
      `${bone}.scale`,
      times,
      keys.flatMap((key) => key.pose.scale),
    ));
  });
  const clip = new THREE.AnimationClip(name || "OSF Animation", project.durationFrames / project.fps, tracks);
  clip.duration = project.durationFrames / project.fps;
  return clip;
}

export function projectKeyframes(project: AnimationProject): Array<{ bone: string; frame: number }> {
  return Object.entries(project.tracks).flatMap(([bone, keys]) =>
    keys.map((key) => ({ bone, frame: key.frame })));
}
