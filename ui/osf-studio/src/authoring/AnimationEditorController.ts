import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import { TransformControls } from "three/examples/jsm/controls/TransformControls.js";
import { decodeRigSkeleton } from "../afDecoder";
import type { RigSource } from "../viewer/AnimationLoader";
import {
  applyBonePose,
  applyProjectFrame,
  captureBonePose,
  type AnimationProject,
  type BonePose,
} from "./AnimationProject";

export interface AuthoringBone {
  name: string;
  depth: number;
  keyed: boolean;
}

export interface AnimationEditorStatus {
  ready: boolean;
  playing: boolean;
  frame: number;
  selectedBone?: string;
  bones: AuthoringBone[];
}

export class AnimationEditorController {
  private readonly scene = new THREE.Scene();
  private readonly camera = new THREE.PerspectiveCamera(42, 1, 0.01, 500);
  private readonly renderer: THREE.WebGLRenderer;
  private readonly orbit: OrbitControls;
  private readonly transform: TransformControls;
  private readonly observer: ResizeObserver;
  private readonly clock = new THREE.Clock();
  private readonly raycaster = new THREE.Raycaster();
  private readonly pointer = new THREE.Vector2();
  private frameRequest = 0;
  private disposed = false;
  private rigRoot?: THREE.Group;
  private skeletonHelper?: THREE.SkeletonHelper;
  private bonePoints?: THREE.Points;
  private bonePointGeometry?: THREE.BufferGeometry;
  private bonePointMaterial?: THREE.PointsMaterial;
  private bones: THREE.Bone[] = [];
  private boneMap = new Map<string, THREE.Bone>();
  private bindPoses = new Map<string, BonePose>();
  private project?: AnimationProject;
  private selected?: THREE.Bone;
  private currentFrame = 0;
  private playing = false;
  private playhead = 0;

  constructor(
    private readonly host: HTMLElement,
    private readonly onStatus: (status: AnimationEditorStatus) => void,
    private readonly onTransformCommitted: (bone: string, pose: BonePose) => void,
  ) {
    try {
      this.renderer = new THREE.WebGLRenderer({ antialias: true });
    } catch {
      throw new Error("WebGL could not be initialized for animation authoring.");
    }
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.scene.background = new THREE.Color(0x090c11);
    this.scene.fog = new THREE.FogExp2(0x090c11, 0.026);
    this.camera.position.set(4, 3, 6);
    this.host.appendChild(this.renderer.domElement);
    this.orbit = new OrbitControls(this.camera, this.renderer.domElement);
    this.orbit.enableDamping = true;
    this.orbit.target.set(0, 1, 0);
    this.transform = new TransformControls(this.camera, this.renderer.domElement);
    this.transform.setMode("rotate");
    this.transform.setSpace("local");
    this.transform.setRotationSnap(THREE.MathUtils.degToRad(1));
    this.scene.add(this.transform.getHelper());
    this.transform.addEventListener("dragging-changed", (event) => {
      this.orbit.enabled = !Boolean(event.value);
      if (event.value) this.setPlaying(false);
    });
    this.transform.addEventListener("mouseUp", () => {
      if (this.selected) this.onTransformCommitted(this.selected.name, captureBonePose(this.selected));
    });
    this.addEnvironment();
    this.renderer.domElement.addEventListener("pointerdown", this.pickBone);
    this.observer = new ResizeObserver(() => this.resize());
    this.observer.observe(host);
    this.resize();
    this.frameRequest = requestAnimationFrame(this.draw);
    this.emit();
  }

  private addEnvironment(): void {
    this.scene.add(new THREE.HemisphereLight(0xb8e8ff, 0x20140e, 2));
    const key = new THREE.DirectionalLight(0xffffff, 2.3);
    key.position.set(4, 7, 5);
    this.scene.add(key);
    const grid = new THREE.GridHelper(20, 40, 0x394451, 0x202731);
    grid.material.transparent = true;
    grid.material.opacity = 0.72;
    this.scene.add(grid);
  }

  private resize(): void {
    const width = Math.max(1, this.host.clientWidth);
    const height = Math.max(1, this.host.clientHeight);
    this.renderer.setSize(width, height, false);
    this.camera.aspect = width / height;
    this.camera.updateProjectionMatrix();
  }

  private draw = () => {
    if (this.disposed) return;
    const delta = Math.min(this.clock.getDelta(), 0.1);
    if (this.playing && this.project) {
      this.playhead += delta * this.project.fps;
      if (this.playhead > this.project.durationFrames) this.playhead = 0;
      const nextFrame = Math.round(this.playhead);
      if (nextFrame !== this.currentFrame) {
        this.currentFrame = nextFrame;
        this.applyFrame();
        this.emit();
      }
    }
    this.updateBonePoints();
    this.orbit.update();
    this.renderer.render(this.scene, this.camera);
    this.frameRequest = requestAnimationFrame(this.draw);
  };

  loadRig(rig: RigSource): void {
    this.unloadRig();
    const decoded = decodeRigSkeleton(rig.bytes.slice(0));
    const rigRoot = decoded.scene;
    this.rigRoot = rigRoot;
    this.bones = decoded.bones;
    this.boneMap = new Map(this.bones.map((bone) => [bone.name, bone]));
    this.bindPoses = new Map(this.bones.map((bone) => [bone.name, captureBonePose(bone)]));
    this.scene.add(rigRoot);
    this.skeletonHelper = new THREE.SkeletonHelper(rigRoot);
    const helperMaterial = this.skeletonHelper.material as THREE.LineBasicMaterial;
    helperMaterial.depthTest = false;
    helperMaterial.transparent = true;
    helperMaterial.opacity = 0.85;
    this.scene.add(this.skeletonHelper);

    this.bonePointGeometry = new THREE.BufferGeometry();
    this.bonePointGeometry.setAttribute("position", new THREE.BufferAttribute(new Float32Array(this.bones.length * 3), 3));
    this.bonePointMaterial = new THREE.PointsMaterial({
      color: 0xff6b36,
      depthTest: false,
      size: 7,
      sizeAttenuation: false,
    });
    this.bonePoints = new THREE.Points(this.bonePointGeometry, this.bonePointMaterial);
    this.bonePoints.renderOrder = 12;
    this.scene.add(this.bonePoints);
    this.updateBonePoints();
    this.fit();
    this.emit();
  }

  setProject(project: AnimationProject): void {
    this.project = project;
    this.currentFrame = Math.min(this.currentFrame, project.durationFrames);
    this.playhead = this.currentFrame;
    this.applyFrame();
    this.emit();
  }

  setFrame(frame: number): void {
    if (!this.project) return;
    this.currentFrame = Math.max(0, Math.min(Math.round(frame), this.project.durationFrames));
    this.playhead = this.currentFrame;
    this.applyFrame();
    this.emit();
  }

  setPlaying(value: boolean): void {
    this.playing = Boolean(value && this.project && this.rigRoot);
    this.emit();
  }

  selectBone(name?: string): void {
    this.selected = name ? this.boneMap.get(name) : undefined;
    if (this.selected) this.transform.attach(this.selected);
    else this.transform.detach();
    this.emit();
  }

  selectedPose(): BonePose | undefined {
    return this.selected ? captureBonePose(this.selected) : undefined;
  }

  selectedBindPose(): BonePose | undefined {
    return this.selected ? structuredClone(this.bindPoses.get(this.selected.name)) : undefined;
  }

  setSelectedPose(pose: BonePose): void {
    if (!this.selected) return;
    applyBonePose(this.selected, pose);
    this.onTransformCommitted(this.selected.name, captureBonePose(this.selected));
  }

  fit(): void {
    if (!this.rigRoot) return;
    this.rigRoot.updateMatrixWorld(true);
    const box = new THREE.Box3().setFromObject(this.skeletonHelper ?? this.rigRoot);
    if (box.isEmpty()) box.set(new THREE.Vector3(-1, 0, -1), new THREE.Vector3(1, 2, 1));
    const center = box.getCenter(new THREE.Vector3());
    const extent = Math.max(...box.getSize(new THREE.Vector3()).toArray(), 0.5);
    this.orbit.target.copy(center);
    this.camera.position.copy(center)
      .addScaledVector(new THREE.Vector3(1.2, 0.75, 1.4).normalize(), extent * 2.2);
    this.camera.near = Math.max(0.001, extent / 100);
    this.camera.far = Math.max(100, extent * 100);
    this.camera.updateProjectionMatrix();
    this.orbit.update();
  }

  private applyFrame(): void {
    if (!this.project) return;
    applyProjectFrame(this.project, this.boneMap, this.bindPoses, this.currentFrame);
    this.rigRoot?.updateMatrixWorld(true);
    this.updateBonePoints();
  }

  private updateBonePoints(): void {
    const attribute = this.bonePointGeometry?.getAttribute("position") as THREE.BufferAttribute | undefined;
    if (!attribute) return;
    const position = new THREE.Vector3();
    this.bones.forEach((bone, index) => {
      bone.getWorldPosition(position);
      attribute.setXYZ(index, position.x, position.y, position.z);
    });
    attribute.needsUpdate = true;
  }

  private pickBone = (event: PointerEvent) => {
    if (!this.bonePoints || this.transform.dragging) return;
    const bounds = this.renderer.domElement.getBoundingClientRect();
    this.pointer.set(
      ((event.clientX - bounds.left) / bounds.width) * 2 - 1,
      -((event.clientY - bounds.top) / bounds.height) * 2 + 1,
    );
    this.raycaster.params.Points!.threshold = 0.08;
    this.raycaster.setFromCamera(this.pointer, this.camera);
    const hit = this.raycaster.intersectObject(this.bonePoints)[0];
    if (hit?.index !== undefined) this.selectBone(this.bones[hit.index]?.name);
  };

  private boneDepth(bone: THREE.Bone): number {
    let depth = 0;
    let parent = bone.parent;
    while (parent instanceof THREE.Bone) {
      depth += 1;
      parent = parent.parent;
    }
    return depth;
  }

  private emit(): void {
    this.onStatus({
      ready: Boolean(this.rigRoot),
      playing: this.playing,
      frame: this.currentFrame,
      selectedBone: this.selected?.name,
      bones: this.bones.map((bone) => ({
        name: bone.name,
        depth: this.boneDepth(bone),
        keyed: Boolean(this.project?.tracks[bone.name]?.length),
      })),
    });
  }

  private unloadRig(): void {
    this.transform.detach();
    this.selected = undefined;
    if (this.rigRoot) this.scene.remove(this.rigRoot);
    if (this.skeletonHelper) {
      this.scene.remove(this.skeletonHelper);
      this.skeletonHelper.geometry.dispose();
      (this.skeletonHelper.material as THREE.Material).dispose();
    }
    if (this.bonePoints) this.scene.remove(this.bonePoints);
    this.bonePointGeometry?.dispose();
    this.bonePointMaterial?.dispose();
    this.rigRoot = undefined;
    this.skeletonHelper = undefined;
    this.bonePoints = undefined;
    this.bonePointGeometry = undefined;
    this.bonePointMaterial = undefined;
    this.bones = [];
    this.boneMap.clear();
    this.bindPoses.clear();
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    cancelAnimationFrame(this.frameRequest);
    this.observer.disconnect();
    this.renderer.domElement.removeEventListener("pointerdown", this.pickBone);
    this.unloadRig();
    this.transform.dispose();
    this.transform.getHelper().removeFromParent();
    this.orbit.dispose();
    this.renderer.dispose();
    this.renderer.forceContextLoss();
    this.renderer.domElement.remove();
  }
}
