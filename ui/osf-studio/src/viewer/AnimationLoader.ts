import * as THREE from "three";
import { GLTFLoader } from "three/examples/jsm/loaders/GLTFLoader.js";
import { decodeAf } from "../afDecoder";
import {
  assertFileSize,
  detectAnimationKind,
  validateEmbeddedGltf,
  validateGlbSignature,
} from "../fileSafety";
import { rememberDiagnosticAsset } from "../diagnostics";
import { sha256 } from "../workspaceRepository";

export interface RigSource {
  name: string;
  bytes: ArrayBuffer;
  sha256: string;
  totalBones?: number;
}

export interface LoadedAnimation {
  root: THREE.Group;
  clips: THREE.AnimationClip[];
  skeleton: {
    formalBones: number;
    animatedTransforms: number;
    meshCount: number;
  };
  source: {
    format: "glb" | "gltf" | "af";
    size: number;
    hashPrefix: string;
    rigHashPrefix?: string;
  };
  dispose(): void;
}

export interface AnimationLoaderOptions {
  rig?: RigSource;
}

function disposeMaterial(material: THREE.Material): void {
  for (const value of Object.values(material)) {
    if (value instanceof THREE.Texture) value.dispose();
  }
  material.dispose();
}

export function disposeObject(root?: THREE.Object3D): void {
  root?.traverse((child) => {
    if (!(child instanceof THREE.Mesh)) return;
    child.geometry.dispose();
    const materials = Array.isArray(child.material) ? child.material : [child.material];
    materials.forEach(disposeMaterial);
    if (child instanceof THREE.SkinnedMesh) child.skeleton.dispose();
  });
}

function metadata(root: THREE.Object3D, clips: THREE.AnimationClip[]) {
  let formalBones = 0;
  let meshCount = 0;
  root.traverse((child) => {
    if (child instanceof THREE.Bone) formalBones += 1;
    if (child instanceof THREE.Mesh) meshCount += 1;
  });
  const animatedTransforms = new Set(
    clips.flatMap((clip) => clip.tracks.map((track) => {
      try {
        return THREE.PropertyBinding.parseTrackName(track.name).nodeName;
      } catch {
        return "";
      }
    })).filter(Boolean),
  ).size;
  return { formalBones, animatedTransforms, meshCount };
}

async function gunzip(bytes: ArrayBuffer): Promise<ArrayBuffer> {
  if (!("DecompressionStream" in globalThis)) {
    throw new Error("Gzip GLB files require DecompressionStream support in the browser.");
  }
  try {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
    return await new Response(stream).arrayBuffer();
  } catch {
    throw new Error("The gzip-compressed GLB is malformed or uses unsupported compression.");
  }
}

export class AnimationLoader {
  async load(file: File, options: AnimationLoaderOptions = {}): Promise<LoadedAnimation> {
    const format = detectAnimationKind(file.name);
    assertFileSize(file, format === "af" ? "af" : "gltf");
    const sourceHash = await sha256(file);
    rememberDiagnosticAsset(`animation:${sourceHash}`, {
      type: format,
      size: file.size,
      hashPrefix: sourceHash.slice(0, 12),
    });

    const loader = new GLTFLoader();
    let root: THREE.Group;
    let clips: THREE.AnimationClip[];
    if (format === "af") {
      if (!options.rig) throw new Error("This AF needs a compatible skeleton.rig before it can be decoded.");
      const decoded = decodeAf(await file.arrayBuffer(), options.rig.bytes, "local.af");
      root = decoded.scene;
      clips = decoded.animations;
    } else if (format === "glb") {
      let bytes = await file.arrayBuffer();
      if (validateGlbSignature(bytes) === "gzip") {
        bytes = await gunzip(bytes);
        validateGlbSignature(bytes);
      }
      const parsed = await loader.parseAsync(bytes, "");
      root = parsed.scene;
      clips = parsed.animations;
    } else {
      const source = await file.text();
      validateEmbeddedGltf(source);
      const parsed = await loader.parseAsync(source, "");
      root = parsed.scene;
      clips = parsed.animations;
    }

    const skeleton = metadata(root, clips);
    let disposed = false;
    return {
      root,
      clips,
      skeleton,
      source: {
        format,
        size: file.size,
        hashPrefix: sourceHash.slice(0, 12),
        rigHashPrefix: options.rig?.sha256.slice(0, 12),
      },
      dispose() {
        if (disposed) return;
        disposed = true;
        disposeObject(root);
      },
    };
  }
}

