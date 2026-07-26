import { useEffect, useMemo, useReducer, useRef, useState } from "preact/hooks";
import { inspectRigMetadata } from "../afDecoder";
import { RigCatalog, type RigDescriptor } from "../viewer/RigCatalog";
import type { RigSource } from "../viewer/AnimationLoader";
import {
  animationProjectReducer,
  captureBonePose,
  createAnimationProject,
  projectKeyframes,
  type AnimationProject,
  type AnimationProjectState,
  type BonePose,
} from "./AnimationProject";
import {
  AnimationEditorController,
  type AnimationEditorStatus,
} from "./AnimationEditorController";
import { exportAnimationProject, validateAndAddExportToLibrary } from "./AnimationExporter";
import { AnimationProjectRepository } from "./AnimationProjectRepository";
import "./authoring.css";

const EMPTY_STATUS: AnimationEditorStatus = {
  ready: false,
  playing: false,
  frame: 0,
  bones: [],
};

function download(file: File): void {
  const url = URL.createObjectURL(file);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = file.name;
  anchor.click();
  URL.revokeObjectURL(url);
}

function rigSource(asset: { name: string; blob: Blob; sha256: string }): Promise<RigSource> {
  return asset.blob.arrayBuffer().then((bytes) => ({
    name: asset.name,
    bytes,
    sha256: asset.sha256,
    totalBones: inspectRigMetadata(bytes).totalBones,
  }));
}

export function AnimationAuthoring({ onOpenLibrary }: { onOpenLibrary?: () => void }) {
  const host = useRef<HTMLDivElement>(null);
  const rigInput = useRef<HTMLInputElement>(null);
  const controller = useRef<AnimationEditorController>();
  const repository = useRef<AnimationProjectRepository>();
  const catalog = useRef(new RigCatalog());
  const activeRig = useRef<RigSource>();
  const frameRef = useRef(0);
  const projectRef = useRef<AnimationProject | null>(null);
  const copiedPose = useRef<BonePose>();
  const [editor, dispatch] = useReducer(animationProjectReducer, {
    project: null,
    past: [],
    future: [],
  } satisfies AnimationProjectState);
  const [projects, setProjects] = useState<AnimationProject[]>([]);
  const [status, setStatus] = useState(EMPTY_STATUS);
  const [rig, setRig] = useState<RigDescriptor>();
  const [servedRigs, setServedRigs] = useState<string[]>([]);
  const [selectedServedRig, setSelectedServedRig] = useState("");
  const [boneSearch, setBoneSearch] = useState("");
  const [selectedKey, setSelectedKey] = useState<number>();
  const [notice, setNotice] = useState("");
  const [error, setError] = useState("");
  const [exporting, setExporting] = useState(false);

  projectRef.current = editor.project;

  useEffect(() => {
    if (!host.current) return;
    controller.current = new AnimationEditorController(
      host.current,
      (next) => {
        frameRef.current = next.frame;
        setStatus(next);
      },
      (bone, pose) => dispatch({ type: "setKey", bone, frame: frameRef.current, pose }),
    );
    let active = true;
    void Promise.all([AnimationProjectRepository.open(), catalog.current.initialize()])
      .then(async ([projectRepository, catalogResult]) => {
        if (!active) {
          projectRepository.close();
          return;
        }
        repository.current = projectRepository;
        setServedRigs(catalogResult.served);
        const existing = await projectRepository.list();
        if (!active) return;
        setProjects(existing);
        if (existing[0]) await openProject(existing[0], projectRepository);
        else if (catalogResult.remembered) activateRig(catalogResult.remembered);
      })
      .catch((reason) => setError(reason instanceof Error ? reason.message : "Animation authoring could not start."));
    return () => {
      active = false;
      controller.current?.dispose();
      controller.current = undefined;
      const projectRepository = repository.current;
      const project = projectRef.current;
      repository.current = undefined;
      if (projectRepository && project) {
        void projectRepository.save(project).finally(() => projectRepository.close());
      } else {
        projectRepository?.close();
      }
    };
  }, []);

  useEffect(() => {
    if (!editor.project || !repository.current) return;
    controller.current?.setProject(editor.project);
    const timer = window.setTimeout(() => {
      void repository.current?.save(editor.project!).then(async () => {
        setProjects(await repository.current!.list());
      }).catch((reason) => setError(reason instanceof Error ? reason.message : "The animation project could not be saved."));
    }, 500);
    return () => window.clearTimeout(timer);
  }, [editor.project]);

  async function openProject(
    project: AnimationProject,
    projectRepository = repository.current,
  ): Promise<void> {
    if (!projectRepository) return;
    setError("");
    const asset = await projectRepository.getRigAsset(project.rigAssetId);
    if (!asset) {
      setError(`The local rig for “${project.title}” is missing. Select the same rig again to continue.`);
      return;
    }
    const source = await rigSource(asset);
    activeRig.current = source;
    const metadata = inspectRigMetadata(source.bytes);
    setRig({
      id: asset.id,
      name: asset.name,
      sha256: asset.sha256,
      size: asset.size,
      totalBones: metadata.totalBones,
      animatedBones: metadata.animatedBones,
      source: "remembered",
    });
    controller.current?.loadRig(source);
    dispatch({ type: "load", project });
    controller.current?.setFrame(0);
    setSelectedKey(undefined);
  }

  function activateRig(result: { descriptor: RigDescriptor; rig: RigSource }): void {
    activeRig.current = result.rig;
    setRig(result.descriptor);
    controller.current?.loadRig(result.rig);
    dispatch({ type: "load", project: createAnimationProject(result.descriptor) });
    controller.current?.setFrame(0);
    setSelectedKey(undefined);
    setNotice("New animation project created from the selected rig.");
  }

  async function chooseManualRig(file?: File): Promise<void> {
    if (!file) return;
    setError("");
    try {
      activateRig(await catalog.current.selectManual(file));
      setSelectedServedRig("");
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "The rig could not be opened.");
    } finally {
      if (rigInput.current) rigInput.current.value = "";
    }
  }

  async function chooseServedRig(name: string): Promise<void> {
    if (!name) return;
    setError("");
    try {
      activateRig(await catalog.current.selectServed(name));
      setSelectedServedRig(name);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "The served rig could not be opened.");
    }
  }

  function addKey(pose?: BonePose): void {
    if (!editor.project || !status.selectedBone) return;
    const nextPose = pose ?? controller.current?.selectedPose();
    if (!nextPose) return;
    dispatch({ type: "setKey", bone: status.selectedBone, frame: status.frame, pose: nextPose });
    setSelectedKey(status.frame);
  }

  async function exportProject(): Promise<void> {
    if (!editor.project || !activeRig.current) return;
    setExporting(true);
    setError("");
    setNotice("");
    try {
      const file = await exportAnimationProject(editor.project, activeRig.current);
      const clip = await validateAndAddExportToLibrary(file);
      download(file);
      setNotice(`Exported ${file.name}, validated it, and added “${clip.title}” to the Clip Library.`);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : "The animation could not be exported.");
    } finally {
      setExporting(false);
    }
  }

  const selectedKeys = editor.project && status.selectedBone
    ? editor.project.tracks[status.selectedBone] ?? []
    : [];
  const allKeys = useMemo(
    () => editor.project ? projectKeyframes(editor.project) : [],
    [editor.project],
  );
  const filteredBones = status.bones.filter((bone) =>
    bone.name.toLowerCase().includes(boneSearch.trim().toLowerCase()));
  const durationSeconds = editor.project
    ? editor.project.durationFrames / editor.project.fps
    : 0;

  return (
    <section class="authoring-workspace">
      <aside class="authoring-rail">
        <header>
          <span class="eyebrow">Local projects</span>
          <h2>Animations</h2>
        </header>
        <div class="authoring-projects">
          {!projects.length && <p>No saved projects yet.</p>}
          {projects.map((project) => (
            <button
              class={editor.project?.id === project.id ? "active" : ""}
              onClick={() => void openProject(project)}
              key={project.id}
            >
              <strong>{project.title || "Untitled animation"}</strong>
              <small>{project.rigName} · {(project.durationFrames / project.fps).toFixed(2)}s</small>
            </button>
          ))}
        </div>
        <section class="authoring-rig-picker">
          <span class="eyebrow">Start from rig</span>
          <select
            aria-label="Served development rig"
            value={selectedServedRig}
            disabled={!servedRigs.length}
            onChange={(event) => void chooseServedRig(event.currentTarget.value)}
          >
            <option value="">{servedRigs.length ? "Select served rig…" : "No served rigs"}</option>
            {servedRigs.map((name) => <option value={name} key={name}>{name}</option>)}
          </select>
          <button class="secondary-button" onClick={() => rigInput.current?.click()}>Choose skeleton.rig</button>
          <input
            ref={rigInput}
            class="visually-hidden"
            type="file"
            accept=".rig"
            onChange={(event) => void chooseManualRig(event.currentTarget.files?.[0])}
          />
        </section>
      </aside>

      <div class="authoring-main">
        <div class="authoring-stage">
          <div ref={host} class="authoring-canvas" />
          {!status.ready && (
            <div class="authoring-empty">
              <span class="eyebrow">Animation authoring</span>
              <h2>Choose a skeleton to begin</h2>
              <p>Create browser-local FK animation clips with keyed bone rotations.</p>
              <button class="primary-button" onClick={() => rigInput.current?.click()}>Choose skeleton.rig</button>
            </div>
          )}
          {status.ready && (
            <>
              <div class="viewport-badges">
                <span>{rig?.totalBones ?? status.bones.length} bones</span>
                <span>{Object.keys(editor.project?.tracks ?? {}).length} keyed</span>
                <span>{status.selectedBone || "No bone selected"}</span>
              </div>
              <div class="viewport-help">Click joint or hierarchy · drag rings to rotate · LMB orbit off gizmo</div>
            </>
          )}
        </div>

        <section class="authoring-timeline" aria-label="Animation timeline">
          <div class="timeline-toolbar">
            <button
              aria-label={status.playing ? "Pause animation" : "Play animation"}
              disabled={!status.ready}
              onClick={() => controller.current?.setPlaying(!status.playing)}
            >{status.playing ? "Ⅱ" : "▶"}</button>
            <button disabled={!status.ready} onClick={() => controller.current?.setFrame(0)}>↤</button>
            <strong>Frame {status.frame}</strong>
            <span>{(status.frame / (editor.project?.fps ?? 30)).toFixed(2)}s / {durationSeconds.toFixed(2)}s</span>
          </div>
          <div class="timeline-track">
            <input
              type="range"
              min={0}
              max={editor.project?.durationFrames ?? 1}
              step={1}
              value={status.frame}
              disabled={!editor.project}
              aria-label="Authoring frame"
              onInput={(event) => controller.current?.setFrame(Number(event.currentTarget.value))}
            />
            <div class="timeline-marks">
              {allKeys.map((key) => (
                <i
                  title={`${key.bone}, frame ${key.frame}`}
                  style={{ left: `${(key.frame / (editor.project?.durationFrames || 1)) * 100}%` }}
                  key={`${key.bone}:${key.frame}`}
                />
              ))}
            </div>
          </div>
        </section>
      </div>

      <aside class="authoring-inspector">
        <header>
          <span class="eyebrow">Animation project</span>
          <h2>{editor.project?.title || "No project"}</h2>
          <p>{rig ? `${rig.name} · ${rig.sha256.slice(0, 12)}` : "Select a local rig"}</p>
        </header>
        {(error || notice) && (
          <div class={error ? "viewer-error" : "authoring-notice"} role={error ? "alert" : "status"}>
            {error || notice}
          </div>
        )}
        {editor.project && (
          <>
            <section class="authoring-settings">
              <label class="field">
                <span>Title</span>
                <input
                  value={editor.project.title}
                  onInput={(event) => dispatch({ type: "rename", title: event.currentTarget.value })}
                />
              </label>
              <div class="field-grid two">
                <label class="field">
                  <span>Frame rate</span>
                  <select
                    value={editor.project.fps}
                    onChange={(event) => dispatch({ type: "setFps", fps: Number(event.currentTarget.value) })}
                  >
                    <option value={24}>24 FPS</option>
                    <option value={30}>30 FPS</option>
                    <option value={60}>60 FPS</option>
                  </select>
                </label>
                <label class="field">
                  <span>Frames</span>
                  <input
                    type="number"
                    min={1}
                    max={18000}
                    value={editor.project.durationFrames}
                    onChange={(event) => dispatch({
                      type: "setDuration",
                      durationFrames: Number(event.currentTarget.value),
                    })}
                  />
                </label>
              </div>
              <div class="authoring-history">
                <button disabled={!editor.past.length} onClick={() => dispatch({ type: "undo" })}>Undo</button>
                <button disabled={!editor.future.length} onClick={() => dispatch({ type: "redo" })}>Redo</button>
                <button onClick={() => controller.current?.fit()}>Frame rig</button>
              </div>
            </section>

            <section class="bone-browser">
              <label class="field">
                <span>Bone hierarchy</span>
                <input
                  type="search"
                  value={boneSearch}
                  placeholder="Filter bones…"
                  onInput={(event) => setBoneSearch(event.currentTarget.value)}
                />
              </label>
              <div class="bone-list">
                {filteredBones.map((bone) => (
                  <button
                    class={status.selectedBone === bone.name ? "active" : ""}
                    style={{ paddingLeft: `${10 + Math.min(bone.depth, 8) * 9}px` }}
                    onClick={() => controller.current?.selectBone(bone.name)}
                    key={bone.name}
                  >
                    <i class={bone.keyed ? "keyed" : ""} />
                    <span>{bone.name}</span>
                  </button>
                ))}
              </div>
            </section>

            <section class="key-editor">
              <span class="eyebrow">Selected bone keys</span>
              <div class="key-actions">
                <button disabled={!status.selectedBone} onClick={() => addKey()}>＋ Key pose</button>
                <button
                  disabled={!status.selectedBone}
                  onClick={() => {
                    copiedPose.current = controller.current?.selectedPose();
                    setNotice(copiedPose.current ? "Pose copied." : "");
                  }}
                >Copy</button>
                <button disabled={!status.selectedBone || !copiedPose.current} onClick={() => {
                  if (copiedPose.current) controller.current?.setSelectedPose(copiedPose.current);
                }}>Paste</button>
                <button disabled={!status.selectedBone} onClick={() => {
                  const pose = controller.current?.selectedBindPose();
                  if (pose) controller.current?.setSelectedPose(pose);
                }}>Bind pose</button>
              </div>
              <div class="key-list">
                {!selectedKeys.length && <small>Select a bone and add its first pose key.</small>}
                {selectedKeys.map((key) => (
                  <button
                    class={selectedKey === key.frame ? "active" : ""}
                    onClick={() => {
                      setSelectedKey(key.frame);
                      controller.current?.setFrame(key.frame);
                    }}
                    key={key.frame}
                  >F{key.frame}</button>
                ))}
              </div>
              <div class="key-actions compact">
                <button
                  disabled={selectedKey === undefined}
                  onClick={() => selectedKey !== undefined && status.selectedBone && dispatch({
                    type: "duplicateKey",
                    bone: status.selectedBone,
                    fromFrame: selectedKey,
                    toFrame: status.frame,
                  })}
                >Duplicate here</button>
                <button
                  disabled={selectedKey === undefined}
                  onClick={() => {
                    if (selectedKey === undefined || !status.selectedBone) return;
                    dispatch({
                      type: "moveKey",
                      bone: status.selectedBone,
                      fromFrame: selectedKey,
                      toFrame: status.frame,
                    });
                    setSelectedKey(status.frame);
                  }}
                >Move here</button>
                <button
                  class="danger-quiet"
                  disabled={!status.selectedBone
                    || !editor.project.tracks[status.selectedBone]?.some((key) => key.frame === status.frame)}
                  onClick={() => status.selectedBone && dispatch({
                    type: "deleteKey",
                    bone: status.selectedBone,
                    frame: status.frame,
                  })}
                >Delete current</button>
              </div>
            </section>

            <section class="authoring-export">
              <button
                class="primary-button"
                disabled={exporting || !Object.keys(editor.project.tracks).length}
                onClick={() => void exportProject()}
              >{exporting ? "Validating export…" : "Export GLB + add to library"}</button>
              {onOpenLibrary && <button class="secondary-button" onClick={onOpenLibrary}>Open Clip Library</button>}
              <small>Exports are reloaded through the production viewer before entering the library.</small>
            </section>
          </>
        )}
      </aside>
    </section>
  );
}

export default AnimationAuthoring;
