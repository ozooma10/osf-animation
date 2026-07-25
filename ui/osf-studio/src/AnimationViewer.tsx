import { useEffect, useRef, useState } from "preact/hooks";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import { GLTFLoader } from "three/examples/jsm/loaders/GLTFLoader.js";
import { decodeAf } from "./afDecoder";

interface ViewerRuntime {
  scene: THREE.Scene;
  camera: THREE.PerspectiveCamera;
  controls: OrbitControls;
  renderer: THREE.WebGLRenderer;
  model?: THREE.Object3D;
  helper?: THREE.SkeletonHelper;
  trackRig?: AnimatedNodeRig;
  mixer?: THREE.AnimationMixer;
  animations: THREE.AnimationClip[];
  action?: THREE.AnimationAction;
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
          const nodeName = THREE.PropertyBinding.parseTrackName(track.name).nodeName;
          if (!nodeName) continue;
          const node = root.getObjectByName(nodeName) ?? root.getObjectByProperty("uuid", nodeName);
          if (node) animated.add(node);
        } catch {
          // One unusual property binding should not hide the rest of the rig.
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
    this.segments = this.nodes
      .filter((node) => node !== root && node.parent)
      .map<[THREE.Object3D, THREE.Object3D]>((node) => [node.parent!, node]);
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

  update() {
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

  dispose() {
    this.lineGeometry.dispose();
    this.pointGeometry.dispose();
    this.lineMaterial.dispose();
    this.pointMaterial.dispose();
  }
}
function disposeObject(root?: THREE.Object3D) {
  root?.traverse((child) => {
    if (!(child instanceof THREE.Mesh)) return;
    child.geometry.dispose();
    const materials = Array.isArray(child.material) ? child.material : [child.material];
    materials.forEach((material) => material.dispose());
  });
}

function fitCamera(runtime: ViewerRuntime) {
  if (!runtime.model) return;
  runtime.model.updateMatrixWorld(true);
  const box = new THREE.Box3().setFromObject(runtime.model);
  const bonePoint = new THREE.Vector3();
  runtime.trackRig?.update();
  if (runtime.trackRig) box.expandByObject(runtime.trackRig.group);
  runtime.model.traverse((child) => {
    if (child instanceof THREE.Bone) box.expandByPoint(child.getWorldPosition(bonePoint));
  });
  if (box.isEmpty()) box.set(new THREE.Vector3(-1, 0, -1), new THREE.Vector3(1, 2, 1));

  const center = box.getCenter(new THREE.Vector3());
  const size = box.getSize(new THREE.Vector3());
  const extent = Math.max(size.x, size.y, size.z, 0.5);
  const direction = new THREE.Vector3(1.2, 0.75, 1.4).normalize();
  runtime.controls.target.copy(center);
  runtime.camera.position.copy(center).addScaledVector(direction, extent * 2.2);
  runtime.camera.near = Math.max(0.001, extent / 100);
  runtime.camera.far = Math.max(100, extent * 100);
  runtime.camera.updateProjectionMatrix();
  runtime.controls.update();
}

export function AnimationViewer() {
  const host = useRef<HTMLDivElement>(null);
  const fileInput = useRef<HTMLInputElement>(null);
  const rigInput = useRef<HTMLInputElement>(null);
  const rigFile = useRef<File>();
  const pendingAf = useRef<File>();
  const runtime = useRef<ViewerRuntime>();
  const playingRef = useRef(true);
  const speedRef = useRef(1);
  const [filename, setFilename] = useState("");
  const [animations, setAnimations] = useState<THREE.AnimationClip[]>([]);
  const [selectedAnimation, setSelectedAnimation] = useState(0);
  const [playing, setPlayingState] = useState(true);
  const [speed, setSpeedState] = useState(1);
  const [time, setTime] = useState(0);
  const [duration, setDuration] = useState(0);
  const [boneCount, setBoneCount] = useState(0);
  const [meshCount, setMeshCount] = useState(0);
  const [error, setError] = useState("");
  const [dragging, setDragging] = useState(false);
  const [rigFilename, setRigFilename] = useState("");
  const [serverRigs, setServerRigs] = useState<string[]>([]);
  const [selectedServerRig, setSelectedServerRig] = useState("");

  function setPlaying(value: boolean) {
    playingRef.current = value;
    setPlayingState(value);
    if (value) runtime.current?.action?.play();
    else if (runtime.current?.action) runtime.current.action.paused = true;
    if (value && runtime.current?.action) runtime.current.action.paused = false;
  }

  function setSpeed(value: number) {
    speedRef.current = value;
    setSpeedState(value);
  }

  function chooseAnimation(index: number) {
    const current = runtime.current;
    if (!current) return;
    current.action?.stop();
    current.action = undefined;
    const clip = current.animations[index];
    setSelectedAnimation(index);
    setTime(0);
    setDuration(clip?.duration ?? 0);
    if (clip && current.mixer) {
      const action = current.mixer.clipAction(clip);
      action.setLoop(THREE.LoopRepeat, Infinity);
      action.clampWhenFinished = false;
      action.play();
      action.paused = !playingRef.current;
      current.action = action;
      current.mixer.update(0);
    }
  }

  async function loadFile(file?: File) {
    if (!file) return;
    const lower = file.name.toLowerCase();
    if (!lower.endsWith(".glb") && !lower.endsWith(".gltf") && !lower.endsWith(".af")) {
      setError("Choose a .glb, self-contained .gltf, or Starfield .af animation file.");
      return;
    }
    if (lower.endsWith(".af") && !rigFile.current) {
      pendingAf.current = file;
      setFilename(file.name);
      setError("This .af needs its matching skeleton.rig. Select the rig to continue.");
      rigInput.current?.click();
      return;
    }
    const current = runtime.current;
    if (!current) return;
    setError("");
    setFilename(file.name);
    const url = URL.createObjectURL(file);
    try {
      const loader = new GLTFLoader();
      let gltf: { scene: THREE.Group; animations: THREE.AnimationClip[] };
      if (lower.endsWith(".af")) {
        const decoded = decodeAf(
          await file.arrayBuffer(),
          await rigFile.current!.arrayBuffer(),
          file.name,
        );
        gltf = { scene: decoded.scene, animations: decoded.animations };
        pendingAf.current = undefined;
      } else if (lower.endsWith(".glb")) {
        let bytes = await file.arrayBuffer();
        const signature = new Uint8Array(bytes, 0, Math.min(2, bytes.byteLength));
        if (signature[0] === 0x1f && signature[1] === 0x8b) {
          const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
          bytes = await new Response(stream).arrayBuffer();
        }
        gltf = await loader.parseAsync(bytes, "");
      } else {
        gltf = await loader.loadAsync(url);
      }
      current.action?.stop();
      current.mixer?.stopAllAction();
      if (current.helper) current.scene.remove(current.helper);
      if (current.trackRig) {
        current.scene.remove(current.trackRig.group);
        current.trackRig.dispose();
        current.trackRig = undefined;
      }
      if (current.model) {
        current.scene.remove(current.model);
        disposeObject(current.model);
      }

      current.model = gltf.scene;
      current.animations = gltf.animations;
      current.mixer = new THREE.AnimationMixer(gltf.scene);
      current.scene.add(gltf.scene);

      let bones = 0;
      let meshes = 0;
      gltf.scene.traverse((child) => {
        if (child instanceof THREE.Bone) bones += 1;
        if (child instanceof THREE.Mesh) {
          meshes += 1;
          child.castShadow = true;
          child.receiveShadow = true;
        }
      });
      if (bones) {
        const helper = new THREE.SkeletonHelper(gltf.scene);
        const helperMaterial = helper.material as THREE.LineBasicMaterial;
        helperMaterial.depthTest = false;
        helperMaterial.transparent = true;
        helperMaterial.opacity = meshes ? 0.72 : 1;
        helper.renderOrder = 10;
        current.helper = helper;
        current.scene.add(helper);
      } else {
        current.helper = undefined;
        const trackRig = new AnimatedNodeRig(gltf.scene, gltf.animations);
        current.trackRig = trackRig;
        current.scene.add(trackRig.group);
        bones = trackRig.nodeCount;
      }

      setAnimations(gltf.animations);
      setBoneCount(bones);
      setMeshCount(meshes);
      setSelectedAnimation(0);
      setTime(0);
      setDuration(gltf.animations[0]?.duration ?? 0);
      chooseAnimation(0);
      current.trackRig?.update();
      fitCamera(current);
      if (!gltf.animations.length) setError("The file loaded, but it contains no animation clips.");
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "The animation could not be loaded.");
    } finally {
      URL.revokeObjectURL(url);
      if (fileInput.current) fileInput.current.value = "";
    }
  }

  async function activateRig(file: File, label: string, serverKey = "") {
    rigFile.current = file;
    setRigFilename(label);
    setSelectedServerRig(serverKey);
    setError("");
    const pending = pendingAf.current;
    if (pending) await loadFile(pending);
  }

  async function loadRig(file?: File) {
    if (!file) return;
    if (file.name.toLowerCase() !== "skeleton.rig" && !file.name.toLowerCase().endsWith(".rig")) {
      setError("Choose a skeleton.rig file.");
      return;
    }
    await activateRig(file, file.name);
    if (rigInput.current) rigInput.current.value = "";
  }

  async function loadServerRig(name: string) {
    if (!name) return;
    try {
      const encoded = name.split("/").map(encodeURIComponent).join("/");
      const response = await fetch(`/__osf/rigs/${encoded}`, { cache: "no-store" });
      if (!response.ok) throw new Error(`Local server returned ${response.status}.`);
      const file = new File([await response.blob()], name.split("/").at(-1) ?? "skeleton.rig");
      await activateRig(file, name, name);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "The served rig could not be loaded.");
    }
  }

  useEffect(() => {
    let active = true;
    void fetch("/__osf/rigs", { cache: "no-store" })
      .then((response) => response.ok ? response.json() : [])
      .then((value: unknown) => {
        if (!active || !Array.isArray(value)) return;
        const rigs = value.filter((entry): entry is string => typeof entry === "string");
        setServerRigs(rigs);
        if (rigs.length === 1) void loadServerRig(rigs[0]);
      })
      .catch(() => { /* Hosted builds simply have no local rig service. */ });
    return () => { active = false; };
  }, []);

  useEffect(() => {
    if (!host.current) return;
    const container = host.current;
    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x090c11);
    scene.fog = new THREE.FogExp2(0x090c11, 0.026);

    const camera = new THREE.PerspectiveCamera(42, 1, 0.01, 500);
    camera.position.set(4, 3, 6);
    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.outputColorSpace = THREE.SRGBColorSpace;
    renderer.shadowMap.enabled = true;
    container.appendChild(renderer.domElement);

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.target.set(0, 1, 0);
    controls.update();

    scene.add(new THREE.HemisphereLight(0xb8e8ff, 0x20140e, 2.1));
    const key = new THREE.DirectionalLight(0xffffff, 2.4);
    key.position.set(4, 7, 5);
    key.castShadow = true;
    scene.add(key);
    const rim = new THREE.DirectionalLight(0xff6b36, 2.2);
    rim.position.set(-4, 2, -3);
    scene.add(rim);

    const grid = new THREE.GridHelper(20, 40, 0x394451, 0x202731);
    grid.material.transparent = true;
    grid.material.opacity = 0.72;
    scene.add(grid);

    runtime.current = { scene, camera, controls, renderer, animations: [] };
    const clock = new THREE.Clock();
    let frame = 0;
    let lastUiUpdate = 0;
    const draw = () => {
      const elapsed = performance.now();
      const delta = Math.min(clock.getDelta(), 0.1);
      const current = runtime.current;
      if (current?.mixer && playingRef.current) current.mixer.update(delta * speedRef.current);
      current?.trackRig?.update();
      current?.controls.update();
      if (current) current.renderer.render(current.scene, current.camera);
      if (elapsed - lastUiUpdate > 80 && current?.action) {
        setTime(current.action.time);
        lastUiUpdate = elapsed;
      }
      frame = requestAnimationFrame(draw);
    };

    const resize = new ResizeObserver(() => {
      const width = Math.max(1, container.clientWidth);
      const height = Math.max(1, container.clientHeight);
      renderer.setSize(width, height, false);
      camera.aspect = width / height;
      camera.updateProjectionMatrix();
    });
    resize.observe(container);
    draw();

    return () => {
      cancelAnimationFrame(frame);
      resize.disconnect();
      controls.dispose();
      disposeObject(runtime.current?.model);
      renderer.dispose();
      renderer.domElement.remove();
      runtime.current = undefined;
    };
  }, []);

  return (
    <section class="viewer-workspace">
      <div
        class={`viewer-stage ${dragging ? "dragging" : ""}`}
        onDragEnter={(event) => { event.preventDefault(); setDragging(true); }}
        onDragOver={(event) => event.preventDefault()}
        onDragLeave={(event) => {
          if (event.currentTarget === event.target) setDragging(false);
        }}
        onDrop={(event) => {
          event.preventDefault();
          setDragging(false);
          void loadFile(event.dataTransfer?.files[0]);
        }}
      >
        <div ref={host} class="viewer-canvas" />
        {!filename && (
          <div class="viewer-empty">
            <div class="viewer-orbit-mark"><span /><span /><b>+</b></div>
            <span class="eyebrow">Animation preview</span>
            <h2>Drop an OSF clip here</h2>
            <p>Open a GLB or Starfield AF to inspect its skeleton, timing, and motion.</p>
            <button class="primary-button" onClick={() => fileInput.current?.click()}>Open animation</button>
            <small>Files stay in your browser and are never uploaded.</small>
          </div>
        )}
        {dragging && <div class="drop-curtain"><strong>Release to load animation</strong></div>}
        {filename && (
          <div class="viewport-badges">
            <span>{boneCount} bones</span>
            <span>{meshCount ? `${meshCount} meshes` : "skeleton only"}</span>
          </div>
        )}
        <div class="viewport-help">LMB orbit · RMB pan · wheel zoom</div>
        <input
          ref={fileInput}
          class="visually-hidden"
          type="file"
          accept=".glb,.gltf,.af,model/gltf-binary,model/gltf+json"
          onChange={(event) => void loadFile(event.currentTarget.files?.[0])}
        />
        <input
          ref={rigInput}
          class="visually-hidden"
          type="file"
          accept=".rig"
          onChange={(event) => void loadRig(event.currentTarget.files?.[0])}
        />
      </div>

      <aside class="viewer-inspector">
        <header>
          <span class="eyebrow">Viewport</span>
          <h2>{filename || "No clip loaded"}</h2>
          <p>{filename ? "Browser-local animation source" : "GLB / GLTF / AF"}</p>
        </header>
        <button class="viewer-open-button" onClick={() => fileInput.current?.click()}>
          <span>＋</span> {filename ? "Open another clip" : "Open animation"}
        </button>
        {error && <div class="viewer-error">{error}</div>}

        <section class="viewer-control-section rig-source">
          <span class="eyebrow">Starfield skeleton</span>
          <label class="field rig-dropdown">
            <span>Served rig</span>
            <select
              value={selectedServerRig}
              disabled={!serverRigs.length}
              onChange={(event) => void loadServerRig(event.currentTarget.value)}
            >
              <option value="">{serverRigs.length ? "Select a local rig" : "No local rigs served"}</option>
              {serverRigs.map((rig) => <option value={rig} key={rig}>{rig}</option>)}
            </select>
          </label>
          <button onClick={() => rigInput.current?.click()}>
            <span>{rigFilename ? "✓" : "＋"}</span>
            <div>
              <strong>{rigFilename || "Choose another skeleton.rig"}</strong>
              <small>{rigFilename ? "Active for AF clips this session" : "Or select a rig file manually"}</small>
            </div>
          </button>
        </section>

        <section class="viewer-control-section">
          <span class="eyebrow">Animation</span>
          <label class="field">
            <span>Embedded clip</span>
            <select
              value={selectedAnimation}
              disabled={!animations.length}
              onChange={(event) => chooseAnimation(Number(event.currentTarget.value))}
            >
              {!animations.length && <option>No animation tracks</option>}
              {animations.map((clip, index) => (
                <option value={index} key={`${clip.name}-${index}`}>{clip.name || `Animation ${index + 1}`}</option>
              ))}
            </select>
          </label>
          <div class="transport">
            <button
              onClick={() => {
                const current = runtime.current;
                if (!current?.action) return;
                current.action.reset();
                current.action.paused = !playingRef.current;
                setTime(0);
              }}
              disabled={!animations.length}
              title="Restart"
            >
              ↤
            </button>
            <button class="play-button" onClick={() => setPlaying(!playing)} disabled={!animations.length}>
              {playing ? "Ⅱ" : "▶"}
            </button>
            <button onClick={() => fitCamera(runtime.current!)} disabled={!filename} title="Frame animation">⌖</button>
          </div>
          <div class="scrubber">
            <input
              type="range"
              min={0}
              max={Math.max(duration, 0.001)}
              step={0.001}
              value={Math.min(time, duration)}
              disabled={!duration}
              onInput={(event) => {
                const next = Number(event.currentTarget.value);
                const action = runtime.current?.action;
                if (action && runtime.current?.mixer) {
                  action.time = next;
                  runtime.current.mixer.update(0);
                  setTime(next);
                }
              }}
              aria-label="Animation time"
            />
            <div><span>{time.toFixed(2)}s</span><span>{duration.toFixed(2)}s</span></div>
          </div>
        </section>

        <section class="viewer-control-section">
          <span class="eyebrow">Playback speed</span>
          <div class="speed-grid">
            {[0.25, 0.5, 1, 1.5, 2].map((value) => (
              <button class={speed === value ? "active" : ""} onClick={() => setSpeed(value)}>{value}×</button>
            ))}
          </div>
        </section>

        <section class="viewer-readout">
          <div><span>Format</span><strong>{filename ? filename.split(".").pop()?.toUpperCase() : "—"}</strong></div>
          <div><span>Animations</span><strong>{animations.length || "—"}</strong></div>
          <div><span>Skeleton</span><strong>{boneCount ? `${boneCount} bones` : "—"}</strong></div>
          <div><span>Geometry</span><strong>{meshCount ? `${meshCount} meshes` : filename ? "None" : "—"}</strong></div>
        </section>
      </aside>
    </section>
  );
}
