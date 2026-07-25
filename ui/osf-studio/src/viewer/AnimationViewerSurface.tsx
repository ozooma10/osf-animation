import { useEffect, useRef, useState } from "preact/hooks";
import { inspectAfMetadata } from "../afDecoder";
import { detectAnimationKind, assertFileSize } from "../fileSafety";
import { recordDiagnostic } from "../diagnostics";
import { ClipLibraryRepository } from "../clipLibrary";
import { AnimationLoader, type RigSource } from "./AnimationLoader";
import { RigCatalog, type RigDescriptor } from "./RigCatalog";
import { ViewerController, type ViewerStatus } from "./ViewerController";

const EMPTY_STATUS: ViewerStatus = {
  ready: false,
  playing: true,
  speed: 1,
  time: 0,
  duration: 0,
  selectedClip: 0,
  clipNames: [],
  formalBones: 0,
  animatedBones: 0,
  meshCount: 0,
};

export function AnimationViewer({ initialClipId }: { initialClipId?: string }) {
  const host = useRef<HTMLDivElement>(null);
  const fileInput = useRef<HTMLInputElement>(null);
  const rigInput = useRef<HTMLInputElement>(null);
  const controller = useRef<ViewerController>();
  const loader = useRef(new AnimationLoader());
  const catalog = useRef(new RigCatalog());
  const generation = useRef(0);
  const pendingAf = useRef<File>();
  const activeRig = useRef<RigSource>();
  const [status, setStatus] = useState(EMPTY_STATUS);
  const [filename, setFilename] = useState("");
  const [error, setError] = useState("");
  const [dragging, setDragging] = useState(false);
  const [servedRigs, setServedRigs] = useState<string[]>([]);
  const [selectedServedRig, setSelectedServedRig] = useState("");
  const [rig, setRig] = useState<RigDescriptor>();
  const [afRequirement, setAfRequirement] = useState<number>();
  const [catalogReady, setCatalogReady] = useState(false);

  useEffect(() => {
    if (!host.current) return;
    controller.current = new ViewerController(host.current, setStatus);
    void catalog.current.initialize().then((result) => {
      setServedRigs(result.served);
      if (result.remembered) {
        activeRig.current = result.remembered.rig;
        setRig(result.remembered.descriptor);
      } else if (result.served.length === 1) {
        void chooseServedRig(result.served[0]);
      }
    }).catch((reason) => {
      recordDiagnostic({
        category: "viewer",
        level: "warning",
        code: "RIG_CATALOG_FAILED",
        message: reason instanceof Error ? reason.message : "Rig catalog initialization failed.",
      });
    });
    return () => {
      generation.current += 1;
      controller.current?.dispose();
      controller.current = undefined;
    };
  }, []);

  async function loadFile(file?: File): Promise<void> {
    if (!file) return;
    const token = ++generation.current;
    setError("");
    setFilename(file.name);
    try {
      const kind = detectAnimationKind(file.name);
      assertFileSize(file, kind === "af" ? "af" : "gltf");
      if (kind === "af") {
        const metadata = inspectAfMetadata(await file.arrayBuffer());
        setAfRequirement(metadata.boneCount);
        if (!activeRig.current) {
          pendingAf.current = file;
          setError("This AF needs its matching skeleton.rig. Choose or remember a rig to continue.");
          rigInput.current?.click();
          return;
        }
      } else {
        setAfRequirement(undefined);
      }
      const loaded = await loader.current.load(file, { rig: activeRig.current });
      if (token !== generation.current) {
        loaded.dispose();
        return;
      }
      controller.current?.load(loaded);
      if (initialClipId) {
        requestAnimationFrame(() => {
          void controller.current?.captureThumbnail().then(async (thumbnail) => {
            if (!thumbnail) return;
            const library = await ClipLibraryRepository.open();
            try { await library.saveThumbnail(initialClipId, thumbnail); } finally { library.close(); }
          }).catch(() => {
            // Preview playback remains available when thumbnail persistence fails.
          });
        });
      }
      pendingAf.current = undefined;
      if (!loaded.clips.length) setError("The file loaded, but it contains no animation clips.");
    } catch (reason) {
      if (token !== generation.current) return;
      const message = reason instanceof Error ? reason.message : "The animation could not be loaded.";
      setError(message);
      recordDiagnostic({ category: "viewer", level: "error", code: "ANIMATION_LOAD_FAILED", message });
    } finally {
      if (fileInput.current) fileInput.current.value = "";
    }
  }

  useEffect(() => {
    if (!initialClipId || !catalogReady) return;
    let active = true;
    void ClipLibraryRepository.open().then(async (library) => {
      try {
        const file = await library.getClipFile(initialClipId);
        if (active && file) await loadFile(file);
        if (active && !file) setError("This library clip is missing its local animation bytes.");
      } finally {
        library.close();
      }
    }).catch((reason) => {
      if (active) setError(reason instanceof Error ? reason.message : "The library clip could not be opened.");
    });
    return () => { active = false; };
  }, [initialClipId, catalogReady]);

  async function activateRig(
    result: { descriptor: RigDescriptor; rig: RigSource },
    servedName = "",
  ): Promise<void> {
    activeRig.current = result.rig;
    setRig(result.descriptor);
    setSelectedServedRig(servedName);

    if (pendingAf.current) await loadFile(pendingAf.current);
  }

  async function chooseManualRig(file?: File): Promise<void> {
    if (!file) return;
    try {
      await activateRig(await catalog.current.selectManual(file));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "The rig could not be loaded.");
    } finally {
      if (rigInput.current) rigInput.current.value = "";
    }
  }

  async function chooseServedRig(name: string): Promise<void> {
    if (!name) return;
    try {
      await activateRig(await catalog.current.selectServed(name), name);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "The served rig could not be loaded.");
    }
  }

  const displayedBones = status.formalBones || status.animatedBones;
  const compatible = afRequirement === undefined || !rig
    ? "Not checked"
    : afRequirement <= rig.animatedBones
      ? "Bone counts compatible"
      : `Incompatible: AF needs ${afRequirement}`;

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
            <p>Open a GLB, embedded glTF, or Starfield AF to inspect its rig and motion.</p>
            <button class="primary-button" onClick={() => fileInput.current?.click()}>Open animation</button>
            <small>Files stay in your browser and are never uploaded.</small>
          </div>
        )}
        {dragging && <div class="drop-curtain"><strong>Release to load animation</strong></div>}
        {filename && (
          <div class="viewport-badges">
            <span>{displayedBones} animated transforms</span>
            <span>{status.meshCount ? `${status.meshCount} meshes` : "skeleton only"}</span>
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
          onChange={(event) => void chooseManualRig(event.currentTarget.files?.[0])}
        />
      </div>

      <aside class="viewer-inspector">
        <header>
          <span class="eyebrow">Viewport</span>
          <h2>{filename || "No clip loaded"}</h2>
          <p>{filename ? "Browser-local animation source" : "GLB / embedded GLTF / AF"}</p>
        </header>
        <button class="viewer-open-button" onClick={() => fileInput.current?.click()}>
          <span>＋</span> {filename ? "Open another clip" : "Open animation"}
        </button>
        {(error || status.error) && <div class="viewer-error" role="alert">{error || status.error}</div>}

        <section class="viewer-control-section rig-source">
          <span class="eyebrow">Starfield skeleton</span>
          <label class="field rig-dropdown">
            <span>Served development rig</span>
            <select
              value={selectedServedRig}
              disabled={!servedRigs.length}
              onChange={(event) => void chooseServedRig(event.currentTarget.value)}
            >
              <option value="">{servedRigs.length ? "Select a local rig" : "No local rigs served"}</option>
              {servedRigs.map((entry) => <option value={entry} key={entry}>{entry}</option>)}
            </select>
          </label>
          <button onClick={() => rigInput.current?.click()}>
            <span>{rig ? "✓" : "＋"}</span>
            <div>
              <strong>{rig?.name || "Choose skeleton.rig"}</strong>
              <small>{rig ? `${rig.source} · ${rig.sha256.slice(0, 12)}` : "Manual rigs are remembered in this browser"}</small>
            </div>
          </button>
          {rig && (
            <div class="rig-metadata" role="status">
              <span>{rig.totalBones} total bones</span>
              <span>{rig.animatedBones} animated bones</span>
              <span>{compatible}</span>
            </div>
          )}
        </section>

        <section class="viewer-control-section">
          <span class="eyebrow">Animation</span>
          <label class="field">
            <span>Embedded clip</span>
            <select
              value={status.selectedClip}
              disabled={!status.clipNames.length}
              onChange={(event) => controller.current?.chooseClip(Number(event.currentTarget.value))}
            >
              {!status.clipNames.length && <option>No animation tracks</option>}
              {status.clipNames.map((name, index) => <option value={index} key={`${name}-${index}`}>{name}</option>)}
            </select>
          </label>
          <div class="transport" aria-label="Viewer transport">
            <button onClick={() => controller.current?.restart()} disabled={!status.ready} title="Restart">↤</button>
            <button
              class="play-button"
              onClick={() => controller.current?.setPlaying(!status.playing)}
              disabled={!status.ready}
              aria-label={status.playing ? "Pause animation" : "Play animation"}
            >{status.playing ? "Ⅱ" : "▶"}</button>
            <button onClick={() => controller.current?.fit()} disabled={!status.ready} title="Frame animation">⌖</button>
          </div>
          <div class="scrubber">
            <input
              type="range"
              min={0}
              max={Math.max(status.duration, 0.001)}
              step={0.001}
              value={Math.min(status.time, status.duration)}
              disabled={!status.duration}
              onInput={(event) => controller.current?.seek(Number(event.currentTarget.value))}
              aria-label="Animation time"
            />
            <div><span>{status.time.toFixed(2)}s</span><span>{status.duration.toFixed(2)}s</span></div>
          </div>
        </section>

        <section class="viewer-control-section">
          <span class="eyebrow">Playback speed</span>
          <div class="speed-grid">
            {[0.25, 0.5, 1, 1.5, 2].map((value) => (
              <button class={status.speed === value ? "active" : ""} onClick={() => controller.current?.setSpeed(value)}>
                {value}×
              </button>
            ))}
          </div>
        </section>

        <section class="viewer-readout">
          <div><span>Format</span><strong>{filename ? filename.split(".").pop()?.toUpperCase() : "—"}</strong></div>
          <div><span>Animations</span><strong>{status.clipNames.length || "—"}</strong></div>
          <div><span>Formal bones</span><strong>{status.formalBones || "—"}</strong></div>
          <div><span>Animated transforms</span><strong>{status.animatedBones || "—"}</strong></div>
        </section>
      </aside>
    </section>
  );
}

