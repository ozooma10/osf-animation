import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import type { LoadedAnimation } from "./AnimationLoader";
import { recordDiagnostic } from "../diagnostics";

export interface ViewerStatus {
  ready: boolean;
  playing: boolean;
  speed: number;
  time: number;
  duration: number;
  selectedClip: number;
  clipNames: string[];
  formalBones: number;
  animatedBones: number;
  meshCount: number;
  error?: string;
}

export class AnimatedNodeRig {
  readonly group = new THREE.Group();
  readonly nodeCount: number;
  private readonly nodes: THREE.Object3D[];
  private readonly segments: Array<[THREE.Object3D, THREE.Object3D]>;
  private readonly linePositions: Float32Array;
  private readonly pointPositions: Float32Array;
  private readonly lineGeometry = new THREE.BufferGeometry();
  private readonly pointGeometry = new THREE.BufferGeometry();
  private readonly lineMaterial = new THREE.LineBasicMaterial({
    color: 0x70d4df, depthTest: false, transparent: true, opacity: 0.95,
  });
  private readonly pointMaterial = new THREE.PointsMaterial({
    color: 0xff6b36, depthTest: false, size: 4, sizeAttenuation: false,
  });
  private readonly scratch = new THREE.Vector3();

  constructor(root: THREE.Object3D, clips: THREE.AnimationClip[]) {
    const animated = new Set<THREE.Object3D>();
    for (const clip of clips) {
      for (const track of clip.tracks) {
        try {
          const name = THREE.PropertyBinding.parseTrackName(track.name).nodeName;
          const node = name && (root.getObjectByName(name) ?? root.getObjectByProperty("uuid", name));
          if (node) animated.add(node);
        } catch {
          // Ignore only the malformed track binding.
        }
      }
    }
    const connected = new Set(animated);
    for (const node of animated) {
      let parent = node.parent;
      while (parent && parent !== root) {
        connected.add(parent);
        parent = parent.parent;
      }
    }
    this.nodes = [...connected];
    this.nodeCount = this.nodes.length;
    this.segments = this.nodes.filter((node) => node !== root && node.parent)
      .map((node) => [node.parent!, node]);
    this.linePositions = new Float32Array(this.segments.length * 6);
    this.pointPositions = new Float32Array(this.nodes.length * 3);
    this.lineGeometry.setAttribute("position", new THREE.BufferAttribute(this.linePositions, 3));
    this.pointGeometry.setAttribute("position", new THREE.BufferAttribute(this.pointPositions, 3));
    const lines = new THREE.LineSegments(this.lineGeometry, this.lineMaterial);
    const points = new THREE.Points(this.pointGeometry, this.pointMaterial);
    lines.frustumCulled = false;
    points.frustumCulled = false;
    lines.renderOrder = 11;
    points.renderOrder = 12;
    this.group.add(lines, points);
    this.update();
  }

  update(): void {
    this.segments.forEach(([parent, child], index) => {
      parent.getWorldPosition(this.scratch);
      this.linePositions.set(this.scratch.toArray(), index * 6);
      child.getWorldPosition(this.scratch);
      this.linePositions.set(this.scratch.toArray(), index * 6 + 3);
    });
    this.nodes.forEach((node, index) => {
      node.getWorldPosition(this.scratch);
      this.pointPositions.set(this.scratch.toArray(), index * 3);
    });
    this.lineGeometry.attributes.position.needsUpdate = true;
    this.pointGeometry.attributes.position.needsUpdate = true;
  }

  dispose(): void {
    this.lineGeometry.dispose();
    this.pointGeometry.dispose();
    this.lineMaterial.dispose();
    this.pointMaterial.dispose();
  }
}

export class ViewerController {
  private readonly scene = new THREE.Scene();
  private readonly camera = new THREE.PerspectiveCamera(42, 1, 0.01, 500);
  private readonly renderer: THREE.WebGLRenderer;
  private readonly controls: OrbitControls;
  private readonly clock = new THREE.Clock();
  private readonly observer: ResizeObserver;
  private frame = 0;
  private disposed = false;
  private loaded?: LoadedAnimation;
  private helper?: THREE.SkeletonHelper;
  private trackRig?: AnimatedNodeRig;
  private mixer?: THREE.AnimationMixer;
  private action?: THREE.AnimationAction;
  private selectedClip = 0;
  private playing = true;
  private speed = 1;
  private lastStatusAt = 0;

  constructor(
    private readonly host: HTMLElement,
    private readonly onStatus: (status: ViewerStatus) => void,
  ) {
    try {
      this.renderer = new THREE.WebGLRenderer({ antialias: true });
    } catch {
      throw new Error("WebGL could not be initialized. Check hardware acceleration and browser graphics settings.");
    }
    this.scene.background = new THREE.Color(0x090c11);
    this.scene.fog = new THREE.FogExp2(0x090c11, 0.026);
    this.camera.position.set(4, 3, 6);
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.renderer.shadowMap.enabled = true;
    this.host.appendChild(this.renderer.domElement);
    this.controls = new OrbitControls(this.camera, this.renderer.domElement);
    this.controls.enableDamping = true;
    this.controls.target.set(0, 1, 0);
    this.controls.update();
    this.addEnvironment();

    this.renderer.domElement.addEventListener("webglcontextlost", this.contextLost, false);
    this.renderer.domElement.addEventListener("webglcontextrestored", this.contextRestored, false);
    this.observer = new ResizeObserver(() => this.resize());
    this.observer.observe(host);
    this.resize();
    this.frame = requestAnimationFrame(this.draw);
    this.emit();
  }

  private contextLost = (event: Event) => {
    event.preventDefault();
    recordDiagnostic({ category: "viewer", level: "error", code: "WEBGL_CONTEXT_LOST", message: "The WebGL context was lost." });
    this.emit("The WebGL context was lost. Retry the viewer surface.");
  };

  private contextRestored = () => {
    recordDiagnostic({ category: "viewer", level: "info", code: "WEBGL_CONTEXT_RESTORED", message: "The WebGL context was restored." });
    this.emit();
  };

  private addEnvironment(): void {
    this.scene.add(new THREE.HemisphereLight(0xb8e8ff, 0x20140e, 2.1));
    const key = new THREE.DirectionalLight(0xffffff, 2.4);
    key.position.set(4, 7, 5);
    this.scene.add(key);
    const rim = new THREE.DirectionalLight(0xff6b36, 2.2);
    rim.position.set(-4, 2, -3);
    this.scene.add(rim);
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
    const now = performance.now();
    const delta = Math.min(this.clock.getDelta(), 0.1);
    if (this.mixer && this.playing) this.mixer.update(delta * this.speed);
    this.trackRig?.update();
    this.controls.update();
    this.renderer.render(this.scene, this.camera);
    if (now - this.lastStatusAt > 80) {
      this.emit();
      this.lastStatusAt = now;
    }
    this.frame = requestAnimationFrame(this.draw);
  };

  load(animation: LoadedAnimation): void {
    this.unload();
    this.loaded = animation;
    this.scene.add(animation.root);
    animation.root.traverse((child) => {
      if (child instanceof THREE.Mesh) {
        child.castShadow = true;
        child.receiveShadow = true;
      }
    });
    this.mixer = new THREE.AnimationMixer(animation.root);
    if (animation.skeleton.formalBones) {
      this.helper = new THREE.SkeletonHelper(animation.root);
      const material = this.helper.material as THREE.LineBasicMaterial;
      material.depthTest = false;
      material.transparent = true;
      material.opacity = animation.skeleton.meshCount ? 0.72 : 1;
      this.scene.add(this.helper);
    } else {
      this.trackRig = new AnimatedNodeRig(animation.root, animation.clips);
      this.scene.add(this.trackRig.group);
    }
    this.chooseClip(0);
    this.fit();
  }

  private unload(): void {
    this.action?.stop();
    this.mixer?.stopAllAction();
    if (this.loaded) {
      this.mixer?.uncacheRoot(this.loaded.root);
      this.scene.remove(this.loaded.root);
    }
    if (this.helper) {
      this.scene.remove(this.helper);
      this.helper.geometry.dispose();
      (this.helper.material as THREE.Material).dispose();
    }
    if (this.trackRig) {
      this.scene.remove(this.trackRig.group);
      this.trackRig.dispose();
    }
    this.loaded?.dispose();
    this.loaded = undefined;
    this.helper = undefined;
    this.trackRig = undefined;
    this.mixer = undefined;
    this.action = undefined;
  }

  chooseClip(index: number): void {
    this.action?.stop();
    this.selectedClip = Math.max(0, index);
    const clip = this.loaded?.clips[this.selectedClip];
    if (clip && this.mixer) {
      this.action = this.mixer.clipAction(clip);
      this.action.setLoop(THREE.LoopRepeat, Infinity);
      this.action.play();
      this.action.paused = !this.playing;
      this.mixer.update(0);
    } else {
      this.action = undefined;
    }
    this.emit();
  }

  setPlaying(value: boolean): void {
    this.playing = value;
    if (this.action) this.action.paused = !value;
    this.emit();
  }

  setSpeed(value: number): void {
    this.speed = value;
    this.emit();
  }

  seek(value: number): void {
    if (!this.action || !this.mixer) return;
    this.action.time = Math.max(0, Math.min(value, this.action.getClip().duration));
    this.mixer.update(0);
    this.emit();
  }

  restart(): void {
    this.action?.reset();
    if (this.action) this.action.paused = !this.playing;
    this.emit();
  }

  fit(): void {
    if (!this.loaded) return;
    this.loaded.root.updateMatrixWorld(true);
    const box = new THREE.Box3().setFromObject(this.loaded.root);
    if (this.trackRig) box.expandByObject(this.trackRig.group);
    if (box.isEmpty()) box.set(new THREE.Vector3(-1, 0, -1), new THREE.Vector3(1, 2, 1));
    const center = box.getCenter(new THREE.Vector3());
    const extent = Math.max(...box.getSize(new THREE.Vector3()).toArray(), 0.5);
    this.controls.target.copy(center);
    this.camera.position.copy(center).addScaledVector(new THREE.Vector3(1.2, 0.75, 1.4).normalize(), extent * 2.2);
    this.camera.near = Math.max(0.001, extent / 100);
    this.camera.far = Math.max(100, extent * 100);
    this.camera.updateProjectionMatrix();
    this.controls.update();
  }

  async captureThumbnail(): Promise<Blob | null> {
    if (!this.loaded || this.disposed) return null;
    this.renderer.render(this.scene, this.camera);
    return new Promise((resolve) => {
      this.renderer.domElement.toBlob((blob) => resolve(blob), "image/webp", 0.82);
    });
  }

  private emit(error?: string): void {
    const clip = this.loaded?.clips[this.selectedClip];
    this.onStatus({
      ready: Boolean(this.loaded),
      playing: this.playing,
      speed: this.speed,
      time: this.action?.time ?? 0,
      duration: clip?.duration ?? 0,
      selectedClip: this.selectedClip,
      clipNames: this.loaded?.clips.map((entry, index) => entry.name || `Animation ${index + 1}`) ?? [],
      formalBones: this.loaded?.skeleton.formalBones ?? 0,
      animatedBones: this.loaded?.skeleton.animatedTransforms ?? 0,
      meshCount: this.loaded?.skeleton.meshCount ?? 0,
      error,
    });
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    cancelAnimationFrame(this.frame);
    this.observer.disconnect();
    this.unload();
    this.controls.dispose();
    this.renderer.domElement.removeEventListener("webglcontextlost", this.contextLost);
    this.renderer.domElement.removeEventListener("webglcontextrestored", this.contextRestored);
    this.renderer.dispose();
    this.renderer.forceContextLoss();
    this.renderer.domElement.remove();
  }
}

