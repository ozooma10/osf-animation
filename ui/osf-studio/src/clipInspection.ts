import * as THREE from "three";
import { AnimationLoader } from "./viewer/AnimationLoader";

export interface ClipInspection {
  durationSeconds?: number;
  animationCount: number;
  boneCount: number;
  thumbnail?: Blob;
}

function canvasBlob(canvas: HTMLCanvasElement): Promise<Blob | undefined> {
  return new Promise((resolve) => {
    canvas.toBlob((blob) => resolve(blob ?? undefined), "image/webp", 0.82);
  });
}

export async function inspectClip(file: File): Promise<ClipInspection> {
  const loaded = await new AnimationLoader().load(file);
  const durationSeconds = loaded.clips.length
    ? Math.max(...loaded.clips.map((clip) => clip.duration))
    : undefined;
  const result: ClipInspection = {
    durationSeconds,
    animationCount: loaded.clips.length,
    boneCount: loaded.skeleton.formalBones || loaded.skeleton.animatedTransforms,
  };

  let renderer: THREE.WebGLRenderer | undefined;
  let helper: THREE.SkeletonHelper | undefined;
  let mixer: THREE.AnimationMixer | undefined;
  try {
    renderer = new THREE.WebGLRenderer({
      antialias: true,
      preserveDrawingBuffer: true,
      powerPreference: "low-power",
    });
    renderer.setPixelRatio(1);
    renderer.setSize(480, 270, false);
    renderer.outputColorSpace = THREE.SRGBColorSpace;
    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x090c11);
    scene.add(new THREE.HemisphereLight(0xb8e8ff, 0x20140e, 2.4));
    const key = new THREE.DirectionalLight(0xffb38f, 2.1);
    key.position.set(3, 6, 4);
    scene.add(key);
    scene.add(loaded.root);

    if (loaded.skeleton.formalBones) {
      helper = new THREE.SkeletonHelper(loaded.root);
      (helper.material as THREE.LineBasicMaterial).color.set(0x70d4df);
      scene.add(helper);
    }
    if (loaded.clips[0]) {
      mixer = new THREE.AnimationMixer(loaded.root);
      const action = mixer.clipAction(loaded.clips[0]);
      action.play();
      action.time = loaded.clips[0].duration * 0.2;
      mixer.update(0);
    }

    loaded.root.updateMatrixWorld(true);
    const box = new THREE.Box3().setFromObject(loaded.root);
    if (helper) box.expandByObject(helper);
    if (box.isEmpty()) box.set(new THREE.Vector3(-1, 0, -1), new THREE.Vector3(1, 2, 1));
    const center = box.getCenter(new THREE.Vector3());
    const extent = Math.max(...box.getSize(new THREE.Vector3()).toArray(), 0.5);
    const camera = new THREE.PerspectiveCamera(38, 480 / 270, Math.max(0.001, extent / 100), Math.max(100, extent * 100));
    camera.position.copy(center).addScaledVector(new THREE.Vector3(1.15, 0.72, 1.35).normalize(), extent * 2.25);
    camera.lookAt(center);
    renderer.render(scene, camera);
    result.thumbnail = await canvasBlob(renderer.domElement);
  } catch {
    // Metadata remains useful when WebGL thumbnail generation is unavailable.
  } finally {
    if (mixer) {
      mixer.stopAllAction();
      mixer.uncacheRoot(loaded.root);
    }
    if (helper) {
      helper.geometry.dispose();
      const materials = Array.isArray(helper.material) ? helper.material : [helper.material];
      materials.forEach((material) => material.dispose());
    }
    renderer?.dispose();
    renderer?.forceContextLoss();
    loaded.dispose();
  }
  return result;
}
