import { lazy, Suspense } from "preact/compat";
import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { ErrorBoundary } from "./ErrorBoundary";
import { downloadDiagnosticReport } from "./diagnostics";
import { WORKSPACE_SCHEMA_VERSION } from "./workspaceRepository";
import { assertFileSize } from "./fileSafety";
import { useWorkspace } from "./useWorkspace";
import { ClipLibraryRepository, type ClipLibrarySnapshot } from "./clipLibrary";
import {
  appendScene,
  exportSceneBundle,
  importSceneBundle,
  resolveSceneDependencies,
  sceneFromLibraryInsertion,
  type LibraryInsertion,
} from "./sceneLibrary";
import {
  SAMPLE_SCENE,
  addScene,
  cloneJson,
  isObject,
  parseDocument,
  removeScene,
  replaceScene,
  sceneAt,
  scenesOf,
  serializeDocument,
  validateDocuments,
  type JsonObject,
  type JsonValue,
  type SceneLocation,
  type StudioDocument,
} from "./model";

type Tab = "form" | "json";
const EMPTY_LIBRARY: ClipLibrarySnapshot = { clips: [], clipSets: [] };
interface SceneEditorProps {
  insertion?: LibraryInsertion;
  onInsertionApplied?: () => void;
}
function text(value: JsonValue | undefined): string {
  return typeof value === "string" ? value : "";
}

function numeric(value: JsonValue | undefined, fallback = 0): number {
  return typeof value === "number" && Number.isFinite(value) ? value : fallback;
}

function object(value: JsonValue | undefined): JsonObject {
  return isObject(value) ? value : {};
}

function list(value: JsonValue | undefined): JsonValue[] {
  return Array.isArray(value) ? value : [];
}

function downloadBlob(filename: string, blob: Blob) {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.click();
  queueMicrotask(() => URL.revokeObjectURL(url));
}

function download(filename: string, contents: string) {
  const blob = new Blob([contents], { type: "application/json;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename.endsWith(".json") ? filename : `${filename}.osf.json`;
  anchor.click();
  URL.revokeObjectURL(url);
}

function NumberField(props: {
  label: string;
  value: number;
  step?: number;
  min?: number;
  onChange: (value: number) => void;
}) {
  return (
    <label class="field compact-field">
      <span>{props.label}</span>
      <input
        type="number"
        value={props.value}
        step={props.step ?? 1}
        min={props.min}
        onInput={(event) => props.onChange(Number(event.currentTarget.value))}
      />
    </label>
  );
}

function Toggle(props: { label: string; hint: string; value: boolean; onChange: (value: boolean) => void }) {
  return (
    <label class="toggle-row">
      <span>
        <strong>{props.label}</strong>
        <small>{props.hint}</small>
      </span>
      <input
        type="checkbox"
        checked={props.value}
        onChange={(event) => props.onChange(event.currentTarget.checked)}
      />
    </label>
  );
}

export function SceneEditor({ insertion, onInsertionApplied }: SceneEditorProps) {
  const workspaceController = useWorkspace();
  const historyState = workspaceController.state;
  const snapshot = historyState.present;
  const [tab, setTab] = useState<Tab>("form");
  const [rawJson, setRawJson] = useState("");
  const [jsonError, setJsonError] = useState("");
  const [notice, setNotice] = useState("Drafts stay on this device");
  const fileInput = useRef<HTMLInputElement>(null);
  const bundleInput = useRef<HTMLInputElement>(null);
  const libraryRepository = useRef<ClipLibraryRepository>();
  const [library, setLibrary] = useState(EMPTY_LIBRARY);

  const selectedDocument = snapshot.documents.find((entry) => entry.id === snapshot.selection.documentId)
    ?? snapshot.documents[0];
  const selectedScene = selectedDocument
    ? sceneAt(selectedDocument.root, snapshot.selection.sceneIndex)
    : undefined;
  const diagnostics = useMemo(() => validateDocuments(snapshot.documents), [snapshot.documents]);
  const selectedDiagnostics = selectedDocument ? diagnostics.get(selectedDocument.id) ?? [] : [];
  const sceneDependencies = useMemo(
    () => selectedScene ? resolveSceneDependencies(selectedScene, library.clips) : [],
    [selectedScene, library.clips],
  );
  const availableDependencies = sceneDependencies.filter((entry) => entry.status === "available").length;
  const missingDependencies = sceneDependencies.filter((entry) => entry.status === "missing").length;
  const incompatibleDependencies = sceneDependencies.filter((entry) => entry.status === "incompatible").length;
  const errorCount = [...diagnostics.values()].flat().filter((entry) => entry.severity === "error").length;
  const warningCount = [...diagnostics.values()].flat().filter((entry) => entry.severity === "warning").length;

  useEffect(() => {
    let active = true;
    void ClipLibraryRepository.open().then(async (repository) => {
      if (!active) { repository.close(); return; }
      libraryRepository.current = repository;
      const loaded = await repository.load();
      if (active) setLibrary(loaded);
    }).catch((error) => setNotice(error instanceof Error ? error.message : "The Clip Library could not be opened."));
    return () => {
      active = false;
      libraryRepository.current?.close();
      libraryRepository.current = undefined;
    };
  }, []);

  useEffect(() => {
    if (!insertion || !selectedDocument) return;
    const scene = sceneFromLibraryInsertion(insertion);
    const appended = appendScene(selectedDocument.root, scene);
    workspaceController.dispatch({ type: "replaceDocument", documentId: selectedDocument.id, root: appended.root });
    workspaceController.dispatch({ type: "select", selection: { documentId: selectedDocument.id, sceneIndex: appended.index } });
    setNotice(`Created scene from ${insertion.type === "clip" ? insertion.clip.title : insertion.clipSet.title}`);
    onInsertionApplied?.();
  }, [insertion]);

  useEffect(() => {
    if (selectedDocument) {
      setRawJson(serializeDocument(selectedDocument));
      setJsonError("");
    }
  }, [selectedDocument?.id, selectedDocument?.root]);

  useEffect(() => {
    const handleShortcut = (event: KeyboardEvent) => {
      if (!(event.ctrlKey || event.metaKey) || event.key.toLowerCase() !== "z") return;
      event.preventDefault();
      workspaceController.dispatch({ type: event.shiftKey ? "redo" : "undo" });
    };
    window.addEventListener("keydown", handleShortcut);
    return () => window.removeEventListener("keydown", handleShortcut);
  }, [workspaceController.dispatch]);

  function replaceDocument(documentId: string, root: JsonObject, message?: string) {
    workspaceController.dispatch({ type: "replaceDocument", documentId, root });
    if (message) setNotice(message);
  }

  function patchScene(patch: Partial<JsonObject>) {
    if (!selectedDocument || !selectedScene) return;
    const editKey = `${selectedDocument.id}:${snapshot.selection.sceneIndex}:${Object.keys(patch).sort().join(",")}`;
    workspaceController.dispatch({
      type: "patchScene",
      documentId: selectedDocument.id,
      sceneIndex: snapshot.selection.sceneIndex,
      patch,
      editKey,
    });
  }

  function undo() {
    workspaceController.dispatch({ type: "undo" });
    setNotice("Undid last change");
  }

  function redo() {
    workspaceController.dispatch({ type: "redo" });
    setNotice("Redid change");
  }

  async function importFiles(files: FileList | null) {
    if (!files?.length) return;
    const imported: StudioDocument[] = [];
    const failures: string[] = [];
    for (const file of Array.from(files)) {
      try {
        assertFileSize(file, "json");
        imported.push(parseDocument(file.name, await file.text()));
      } catch (error) {
        failures.push(error instanceof Error ? error.message : "Invalid JSON");
      }
    }
    if (imported.length) {
      workspaceController.dispatch({ type: "importDocuments", documents: imported });
      setNotice(`Imported ${imported.length} file${imported.length === 1 ? "" : "s"}`);
    }
    if (failures.length) setNotice(failures.join(" · "));
    if (fileInput.current) fileInput.current.value = "";
  }

  async function importBundle(file?: File) {
    if (!file || !libraryRepository.current) return;
    try {
      const result = await importSceneBundle(file, libraryRepository.current);
      workspaceController.dispatch({ type: "importDocuments", documents: [result.document] });
      setLibrary(await libraryRepository.current.load());
      setNotice(`Imported scene bundle with ${result.importedClips} clip${result.importedClips === 1 ? "" : "s"}`);
    } catch (error) {
      setNotice(error instanceof Error ? error.message : "The scene bundle could not be imported.");
    } finally {
      if (bundleInput.current) bundleInput.current.value = "";
    }
  }

  async function exportBundle() {
    if (!selectedDocument || !libraryRepository.current) return;
    try {
      const bundledDocument = Array.isArray(selectedDocument.root.scenes) && selectedScene
        ? { ...selectedDocument, root: { ...selectedDocument.root, scenes: [selectedScene] } }
        : selectedDocument;
      const blob = await exportSceneBundle(bundledDocument, library, libraryRepository.current);
      downloadBlob(selectedDocument.filename.replace(/\.osf\.json$/i, "") + ".osfscene", blob);
      setNotice("Exported portable scene bundle");
    } catch (error) {
      setNotice(error instanceof Error ? error.message : "The scene bundle could not be exported.");
    }
  }

  function newFile() {
    workspaceController.dispatch({ type: "newDocument" });
    setNotice("Created a new scene file");
  }

  function deleteCurrentFile() {
    if (!selectedDocument) return;
    workspaceController.dispatch({ type: "removeDocument", documentId: selectedDocument.id });
    setNotice("Removed file from this workspace");
  }

  function addCurrentScene() {
    if (!selectedDocument) return;
    workspaceController.dispatch({ type: "addScene", documentId: selectedDocument.id });
    setNotice("Added scene");
  }

  function deleteCurrentScene() {
    if (!selectedDocument) return;
    workspaceController.dispatch({
      type: "removeScene",
      documentId: selectedDocument.id,
      sceneIndex: snapshot.selection.sceneIndex,
    });
    setNotice("Removed scene");
  }

  function applyRawJson() {
    if (!selectedDocument) return;
    try {
      const parsed = parseDocument(selectedDocument.filename, rawJson);
      replaceDocument(selectedDocument.id, parsed.root, "Applied JSON changes");
      setJsonError("");
    } catch (error) {
      setJsonError(error instanceof Error ? error.message : "Invalid JSON");
    }
  }
  function updateRoles(nextRoles: JsonValue[]) {
    patchScene({ roles: nextRoles });
  }

  function updateStages(nextStages: JsonValue[]) {
    if (!selectedDocument || !selectedScene) return;
    const nextScene: JsonObject = { ...selectedScene, stages: nextStages };
    delete nextScene["clip"];
    replaceDocument(
      selectedDocument.id,
      replaceScene(selectedDocument.root, snapshot.selection.sceneIndex, nextScene),
    );
  }

  const bareClipFile = typeof selectedScene?.clip === "string" ? selectedScene.clip : undefined;
  const authoredRoles = list(selectedScene?.roles);
  const roles: JsonValue[] = authoredRoles.length ? authoredRoles : bareClipFile ? [{}] : authoredRoles;
  const stages: JsonValue[] = bareClipFile ? [{ clips: [bareClipFile] }] : list(selectedScene?.stages);
  const graphScene = Array.isArray(selectedScene?.nodes);
  return (
    <>
      <div class="editor-actions-bar">
        <div class="top-actions">
            <span class="status-copy">{notice} · {workspaceController.persistenceStatus}</span>
            <button class="icon-button" onClick={undo} disabled={!historyState.past.length} title="Undo (Ctrl+Z)">↶</button>
            <button class="icon-button" onClick={redo} disabled={!historyState.future.length} title="Redo (Ctrl+Shift+Z)">↷</button>
            <input
              ref={bundleInput}
              class="visually-hidden"
              type="file"
              accept=".osfscene,.zip"
              onChange={(event) => void importBundle(event.currentTarget.files?.[0])}
            />
            <input
              ref={fileInput}
              class="visually-hidden"
              type="file"
              accept=".json,.osf.json"
              multiple
              onChange={(event) => void importFiles(event.currentTarget.files)}
            />
            <button class="secondary-button" onClick={() => fileInput.current?.click()}>Import JSON</button>
            <button class="secondary-button" onClick={() => bundleInput.current?.click()}>Import bundle</button>
            <button
              class="primary-button"
              onClick={() => {
                if (!selectedDocument) return;
                download(selectedDocument.filename, serializeDocument(selectedDocument));
                workspaceController.dispatch({ type: "markExported", documentId: selectedDocument.id });
                setNotice("Exported JSON");
              }}
              disabled={!selectedDocument}
            >
              Export JSON
            </button>
            <button class="secondary-button" onClick={() => void exportBundle()} disabled={!selectedDocument}>Export bundle</button>
            <button class="secondary-button" onClick={() => void workspaceController.exportBackup()}>Backup</button>
            <button class="icon-button" title="Export diagnostics" aria-label="Export diagnostics" onClick={() => downloadDiagnosticReport(WORKSPACE_SCHEMA_VERSION)}>ⓘ</button>
          </div>
      </div>
      {workspaceController.storageWarning && <div class="storage-warning" role="status">{workspaceController.storageWarning}</div>}
      <ErrorBoundary scope="editor"><section class="workspace">
        <aside class="rail">
          <div class="rail-heading">
            <div>
              <span class="eyebrow">Workspace</span>
              <h2>Scene files</h2>
            </div>
            <button class="add-button" onClick={newFile} aria-label="New file">+</button>
          </div>
          <div class="file-list">
            {snapshot.documents.map((document) => {
              const active = document.id === selectedDocument?.id;
              const fileIssues = diagnostics.get(document.id) ?? [];
              return (
                <div class={`file-group ${active ? "active" : ""}`} key={document.id}>
                  <button
                    class="file-row"
                    onClick={() => workspaceController.dispatch({ type: "select", selection: { documentId: document.id, sceneIndex: 0 } })}
                  >
                    <span class="file-glyph">{fileIssues.some((item) => item.severity === "error") ? "!" : "{}"}</span>
                    <span class="file-name">{document.filename}</span>
                    {document.dirty && <span class="dirty-dot" title="Unsaved changes" />}
                  </button>
                  {active && (
                    <div class="scene-list">
                      {scenesOf(document.root).map((scene, index) => (
                        <button
                          class={index === snapshot.selection.sceneIndex ? "selected" : ""}
                          onClick={() => workspaceController.dispatch({ type: "select", selection: { documentId: document.id, sceneIndex: index } })}
                          key={`${text(scene.id)}-${index}`}
                        >
                          <span>{String(index + 1).padStart(2, "0")}</span>
                          <strong>{text(scene.name) || text(scene.id) || "Untitled scene"}</strong>
                        </button>
                      ))}
                      <button class="new-scene-row" onClick={addCurrentScene}>＋ Add scene</button>
                    </div>
                  )}
                </div>
              );
            })}
          </div>
          <div class="rail-footer">
            <span>Local workspace</span>
            <strong>{snapshot.documents.length} file{snapshot.documents.length === 1 ? "" : "s"}</strong>
          </div>
        </aside>

        <section class="editor">
          {selectedDocument && selectedScene ? (
            <>
              <header class="editor-header">
                <div>
                  <div class="breadcrumb">
                    <span>{selectedDocument.filename}</span><b>/</b><span>{text(selectedScene.id) || "untitled"}</span>
                  </div>
                  <h2>{text(selectedScene.name) || "Untitled scene"}</h2>
                </div>
                <div class="header-tools">
                  <div class="segmented" role="tablist">
                    <button class={tab === "form" ? "active" : ""} onClick={() => setTab("form")}>Form</button>
                    <button class={tab === "json" ? "active" : ""} onClick={() => setTab("json")}>JSON</button>
                  </div>
                  <button class="danger-quiet" onClick={deleteCurrentScene} disabled={scenesOf(selectedDocument.root).length <= 1}>
                    Delete scene
                  </button>
                </div>
              </header>

              {tab === "json" ? (
                <section class="json-workspace">
                  <div class="section-title">
                    <div>
                      <span class="eyebrow">Source</span>
                      <h3>{selectedDocument.filename}</h3>
                    </div>
                    <button class="primary-button" onClick={applyRawJson}>Apply JSON</button>
                  </div>
                  {jsonError && <div class="inline-error">{jsonError}</div>}
                  <textarea
                    class="json-editor"
                    spellcheck={false}
                    value={rawJson}
                    onInput={(event) => {
                      setRawJson(event.currentTarget.value);
                      setJsonError("");
                    }}
                    aria-label="JSON source"
                  />
                </section>
              ) : graphScene ? (
                <section class="empty-state">
                  <div class="empty-symbol">◇</div>
                  <span class="eyebrow">Graph scene detected</span>
                  <h3>Your graph is safe.</h3>
                  <p>Visual node editing is planned for the next milestone. For now, edit this scene in JSON mode; all graph fields remain untouched.</p>
                  <button class="primary-button" onClick={() => setTab("json")}>Open JSON editor</button>
                </section>
              ) : (
                <div class="form-scroll">
                  <section class="panel hero-panel">
                    <div class="section-title">
                      <div>
                        <span class="eyebrow">Identity</span>
                        <h3>Scene details</h3>
                      </div>
                      <span class="schema-pill">SCHEMA 1</span>
                    </div>
                    <div class="field-grid two">
                      <label class="field">
                        <span>Scene ID</span>
                        <input value={text(selectedScene.id)} onInput={(event) => patchScene({ id: event.currentTarget.value })} />
                      </label>
                      <label class="field">
                        <span>Display name</span>
                        <input value={text(selectedScene.name)} onInput={(event) => patchScene({ name: event.currentTarget.value })} />
                      </label>
                      <label class="field span-two">
                        <span>Tags <small>comma separated</small></span>
                        <input
                          value={list(selectedScene.tags).filter((item): item is string => typeof item === "string").join(", ")}
                          onInput={(event) => patchScene({
                            tags: event.currentTarget.value.split(",").map((item) => item.trim()).filter(Boolean),
                          })}
                          placeholder="solo, idle, romance"
                        />
                      </label>
                    </div>
                    <div class="toggle-grid">
                      <Toggle
                        label="Lock player"
                        hint="Disable player input while participating"
                        value={selectedScene.lockPlayer !== false}
                        onChange={(value) => patchScene({ lockPlayer: value })}
                      />
                      <Toggle
                        label="Strip actors"
                        hint="Hide participant apparel during playback"
                        value={selectedScene.stripActors !== false}
                        onChange={(value) => patchScene({ stripActors: value })}
                      />
                      <Toggle
                        label="Start fade"
                        hint="Use a fade-to-black opening curtain"
                        value={selectedScene.fade === true}
                        onChange={(value) => patchScene({ fade: value })}
                      />
                      <Toggle
                        label="In place"
                        hint="Keep each actor at their current position"
                        value={selectedScene.inPlace === true}
                        onChange={(value) => patchScene({ inPlace: value })}
                      />
                    </div>
                  </section>

                  <section class="panel">
                    <div class="section-title">
                      <div>
                        <span class="eyebrow">Cast</span>
                        <h3>Roles</h3>
                      </div>
                      <button
                        class="secondary-button"
                        onClick={() => updateRoles([...roles, { name: `role${roles.length + 1}`, gender: "any" }])}
                      >
                        ＋ Add role
                      </button>
                    </div>
                    <div class="card-stack">
                      {roles.map((roleValue, roleIndex) => {
                        const role = object(roleValue);
                        const offset = object(role.offset);
                        const setRole = (patch: Partial<JsonObject>) => {
                          const next = [...roles];
                          next[roleIndex] = { ...role, ...patch };
                          updateRoles(next);
                        };
                        return (
                          <article class="role-card" key={roleIndex}>
                            <div class="card-index">{String(roleIndex + 1).padStart(2, "0")}</div>
                            <div class="role-fields">
                              <label class="field">
                                <span>Role name</span>
                                <input value={text(role.name)} onInput={(event) => setRole({ name: event.currentTarget.value })} placeholder="anonymous" />
                              </label>
                              <label class="field">
                                <span>Gender</span>
                                <select value={text(role.gender) || "any"} onChange={(event) => setRole({ gender: event.currentTarget.value })}>
                                  <option value="any">Any</option>
                                  <option value="male">Male</option>
                                  <option value="female">Female</option>
                                </select>
                              </label>
                              <div class="placement-row">
                                <NumberField label="X" value={numeric(offset.x)} step={0.1} onChange={(value) => setRole({ offset: { ...offset, x: value } })} />
                                <NumberField label="Y" value={numeric(offset.y)} step={0.1} onChange={(value) => setRole({ offset: { ...offset, y: value } })} />
                                <NumberField label="Z" value={numeric(offset.z)} step={0.1} onChange={(value) => setRole({ offset: { ...offset, z: value } })} />
                                <NumberField label="Heading" value={numeric(offset.heading)} step={1} onChange={(value) => setRole({ offset: { ...offset, heading: value } })} />
                              </div>
                            </div>
                            <button
                              class="remove-button"
                              onClick={() => updateRoles(roles.filter((_, index) => index !== roleIndex))}
                              disabled={roles.length <= 1}
                              aria-label={`Remove role ${roleIndex + 1}`}
                            >
                              ×
                            </button>
                          </article>
                        );
                      })}
                    </div>
                  </section>

                  <section class="panel">
                    <div class="section-title">
                      <div>
                        <span class="eyebrow">Timeline</span>
                        <h3>Stages</h3>
                      </div>
                      <button
                        class="secondary-button"
                        onClick={() => updateStages([...stages, {
                          name: `Stage ${stages.length + 1}`,
                          loops: 0,
                          clips: roles.map(() => ({ file: "OSF/Animations/clip.glb" })),
                        }])}
                      >
                        ＋ Add stage
                      </button>
                    </div>
                    <div class="timeline">
                      {stages.map((stageValue, stageIndex) => {
                        const stage = Array.isArray(stageValue) ? { clips: stageValue } : object(stageValue);
                        const clips = list(stage.clips);
                        const setStage = (patch: Partial<JsonObject>) => {
                          const next = [...stages];
                          next[stageIndex] = { ...stage, ...patch };
                          updateStages(next);
                        };
                        return (
                          <article class="stage-card" key={stageIndex}>
                            <div class="timeline-node">{stageIndex + 1}</div>
                            <div class="stage-body">
                              <div class="stage-heading">
                                <label class="field stage-name">
                                  <span>Stage name</span>
                                  <input value={text(stage.name)} onInput={(event) => setStage({ name: event.currentTarget.value })} placeholder={`Stage ${stageIndex + 1}`} />
                                </label>
                                <NumberField label="Timer (sec)" value={numeric(stage.timer)} min={0} step={0.1} onChange={(value) => setStage({ timer: value })} />
                                <NumberField label="Loops" value={numeric(stage.loops)} min={0} onChange={(value) => setStage({ loops: Math.max(0, Math.round(value)) })} />
                                <button
                                  class="remove-button"
                                  onClick={() => updateStages(stages.filter((_, index) => index !== stageIndex))}
                                  disabled={stages.length <= 1}
                                  aria-label={`Remove stage ${stageIndex + 1}`}
                                >
                                  ×
                                </button>
                              </div>
                              <div class="clip-list">
                                {clips.map((clipValue, clipIndex) => {
                                  const clip = typeof clipValue === "string" ? { file: clipValue } : object(clipValue);
                                  const dependency = resolveSceneDependencies({ clip: text(clip.file) }, library.clips)[0];
                                  const nextClips = (patch: Partial<JsonObject>) => {
                                    const next = [...clips];
                                    next[clipIndex] = { ...clip, ...patch };
                                    setStage({ clips: next });
                                  };
                                  return (
                                    <div class="clip-row" key={clipIndex}>
                                      <span class="clip-role">{text(object(roles[clipIndex]).name) || `Role ${clipIndex + 1}`}</span>
                                      <label class="field clip-path">
                                        <span>Clip file</span>
                                        <input value={text(clip.file)} onInput={(event) => nextClips({ file: event.currentTarget.value })} />
                                      </label>
                                      <label class="field library-clip-select">
                                        <span>Library <small class={dependency?.status === "available" ? "dependency-ok" : "dependency-missing"}>{dependency?.status ?? "untracked"}</small></span>
                                        <select
                                          value={dependency?.clip?.id ?? ""}
                                          onChange={(event) => {
                                            const selected = library.clips.find((entry) => entry.id === event.currentTarget.value);
                                            if (selected) nextClips({ file: selected.gamePath, sec: selected.durationSeconds });
                                          }}
                                        >
                                          <option value="">Manual path</option>
                                          {library.clips.map((entry) => <option value={entry.id}>{entry.title}</option>)}
                                        </select>
                                      </label>
                                      <label class="field anim-id">
                                        <span>Animation ID</span>
                                        <input value={text(clip.anim)} onInput={(event) => nextClips({ anim: event.currentTarget.value })} placeholder="optional" />
                                      </label>
                                    </div>
                                  );
                                })}
                              </div>
                              <div class="lane-preview">
                                <span>Tracks</span>
                                {(["cue", "action", "sound", "camera"] as const).map((lane) => (
                                  <button
                                    class={Array.isArray(stage[lane]) && stage[lane].length ? "populated" : ""}
                                    onClick={() => setTab("json")}
                                    title="Track editing is currently available in JSON mode"
                                  >
                                    {lane} <b>{Array.isArray(stage[lane]) ? stage[lane].length : 0}</b>
                                  </button>
                                ))}
                              </div>
                            </div>
                          </article>
                        );
                      })}
                    </div>
                  </section>
                </div>
              )}
            </>
          ) : (
            <section class="empty-state"><h3>No scene selected</h3></section>
          )}
        </section>

        <aside class="inspector">
          <div class="inspector-heading">
            <span class="eyebrow">Health</span>
            <h2>Validation</h2>
            <div class="health-summary">
              <span class={errorCount ? "error" : "clear"}>{errorCount} errors</span>
              <span class={warningCount ? "warning" : "clear"}>{warningCount} warnings</span>
            </div>
            <div class="dependency-summary">
              <span>{availableDependencies} library ready</span>
              <span class={missingDependencies ? "missing" : ""}>{missingDependencies} untracked</span>
              <span class={incompatibleDependencies ? "missing" : ""}>{incompatibleDependencies} incompatible</span>
            </div>
          </div>
          <div class="diagnostic-list">
            {!selectedDiagnostics.length ? (
              <div class="all-clear">
                <div>✓</div>
                <strong>Ready to export</strong>
                <p>No structural issues found in this file.</p>
              </div>
            ) : selectedDiagnostics.map((diagnostic, index) => (
              <article class={`diagnostic ${diagnostic.severity}`} key={`${diagnostic.path}-${index}`}>
                <span>{diagnostic.severity === "error" ? "!" : "△"}</span>
                <div>
                  <code>{diagnostic.path}</code>
                  <p>{diagnostic.message}</p>
                </div>
              </article>
            ))}
          </div>
          <div class="inspector-footer">
            <button class="danger-quiet" onClick={deleteCurrentFile} disabled={snapshot.documents.length === 1}>Remove file</button>
            <p>Studio validation checks structure. OSF remains authoritative for game forms and installed assets.</p>
          </div>
        </aside>
      </section></ErrorBoundary>
    </>
  );
}
