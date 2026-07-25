import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { AnimationViewer } from "./AnimationViewer";
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
interface Snapshot {
  documents: StudioDocument[];
  selection: SceneLocation;
}

const STORAGE_KEY = "osf-studio.workspace.v1";

function initialSnapshot(): Snapshot {
  try {
    const stored = localStorage.getItem(STORAGE_KEY);
    if (stored) {
      const documents = JSON.parse(stored) as StudioDocument[];
      if (Array.isArray(documents) && documents.length) {
        return {
          documents,
          selection: { documentId: documents[0].id, sceneIndex: 0 },
        };
      }
    }
  } catch {
    // Corrupt drafts never prevent Studio from starting.
  }
  const document: StudioDocument = {
    id: crypto.randomUUID(),
    filename: "my-pack.osf.json",
    root: cloneJson(SAMPLE_SCENE),
    dirty: false,
  };
  return { documents: [document], selection: { documentId: document.id, sceneIndex: 0 } };
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

export function App() {
  const [surface, setSurface] = useState<"editor" | "viewer">("editor");
  const [history, setHistory] = useState<Snapshot[]>([]);
  const [snapshot, setSnapshot] = useState<Snapshot>(initialSnapshot);
  const [future, setFuture] = useState<Snapshot[]>([]);
  const [tab, setTab] = useState<Tab>("form");
  const [rawJson, setRawJson] = useState("");
  const [jsonError, setJsonError] = useState("");
  const [notice, setNotice] = useState("Drafts stay on this device");
  const fileInput = useRef<HTMLInputElement>(null);

  const selectedDocument = snapshot.documents.find((entry) => entry.id === snapshot.selection.documentId)
    ?? snapshot.documents[0];
  const selectedScene = selectedDocument
    ? sceneAt(selectedDocument.root, snapshot.selection.sceneIndex)
    : undefined;
  const diagnostics = useMemo(() => validateDocuments(snapshot.documents), [snapshot.documents]);
  const selectedDiagnostics = selectedDocument ? diagnostics.get(selectedDocument.id) ?? [] : [];
  const errorCount = [...diagnostics.values()].flat().filter((entry) => entry.severity === "error").length;
  const warningCount = [...diagnostics.values()].flat().filter((entry) => entry.severity === "warning").length;

  useEffect(() => {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(snapshot.documents));
  }, [snapshot.documents]);

  useEffect(() => {
    if (selectedDocument) {
      setRawJson(serializeDocument(selectedDocument));
      setJsonError("");
    }
  }, [selectedDocument?.id, selectedDocument?.root]);

  function commit(next: Snapshot, message?: string) {
    setHistory((items) => [...items.slice(-49), snapshot]);
    setSnapshot(next);
    setFuture([]);
    if (message) setNotice(message);
  }

  function replaceDocument(documentId: string, root: JsonObject, message?: string) {
    commit({
      ...snapshot,
      documents: snapshot.documents.map((document) => document.id === documentId
        ? { ...document, root, dirty: true }
        : document),
    }, message);
  }

  function patchScene(patch: Partial<JsonObject>) {
    if (!selectedDocument || !selectedScene) return;
    replaceDocument(
      selectedDocument.id,
      replaceScene(selectedDocument.root, snapshot.selection.sceneIndex, { ...selectedScene, ...patch }),
    );
  }

  function undo() {
    const prior = history.at(-1);
    if (!prior) return;
    setFuture((items) => [snapshot, ...items].slice(0, 50));
    setSnapshot(prior);
    setHistory((items) => items.slice(0, -1));
    setNotice("Undid last change");
  }

  function redo() {
    const next = future[0];
    if (!next) return;
    setHistory((items) => [...items, snapshot].slice(-50));
    setSnapshot(next);
    setFuture((items) => items.slice(1));
    setNotice("Redid change");
  }

  async function importFiles(files: FileList | null) {
    if (!files?.length) return;
    const imported: StudioDocument[] = [];
    const failures: string[] = [];
    for (const file of Array.from(files)) {
      try {
        imported.push(parseDocument(file.name, await file.text()));
      } catch (error) {
        failures.push(`${file.name}: ${error instanceof Error ? error.message : "Invalid JSON"}`);
      }
    }
    if (imported.length) {
      commit({
        documents: [...snapshot.documents, ...imported],
        selection: { documentId: imported[0].id, sceneIndex: 0 },
      }, `Imported ${imported.length} file${imported.length === 1 ? "" : "s"}`);
    }
    if (failures.length) setNotice(failures.join(" · "));
    if (fileInput.current) fileInput.current.value = "";
  }

  function newFile() {
    const document: StudioDocument = {
      id: crypto.randomUUID(),
      filename: "untitled.osf.json",
      root: cloneJson(SAMPLE_SCENE),
      dirty: true,
    };
    commit({
      documents: [...snapshot.documents, document],
      selection: { documentId: document.id, sceneIndex: 0 },
    }, "Created a new scene file");
  }

  function deleteCurrentFile() {
    if (!selectedDocument || snapshot.documents.length === 1) return;
    const documents = snapshot.documents.filter((entry) => entry.id !== selectedDocument.id);
    commit({
      documents,
      selection: { documentId: documents[0].id, sceneIndex: 0 },
    }, "Removed file from this workspace");
  }

  function addCurrentScene() {
    if (!selectedDocument) return;
    const result = addScene(selectedDocument.root);
    commit({
      documents: snapshot.documents.map((document) => document.id === selectedDocument.id
        ? { ...document, root: result.root, dirty: true }
        : document),
      selection: { documentId: selectedDocument.id, sceneIndex: result.index },
    }, "Added scene");
  }

  function deleteCurrentScene() {
    if (!selectedDocument || scenesOf(selectedDocument.root).length <= 1) return;
    const index = snapshot.selection.sceneIndex;
    commit({
      documents: snapshot.documents.map((document) => document.id === selectedDocument.id
        ? { ...document, root: removeScene(document.root, index), dirty: true }
        : document),
      selection: { documentId: selectedDocument.id, sceneIndex: Math.max(0, index - 1) },
    }, "Removed scene");
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
    <main class="studio-shell">
      <header class="topbar">
        <div class="brand">
          <div class="brand-mark">OSF</div>
          <div>
            <h1>Studio</h1>
            <p>{surface === "editor" ? "Scene authoring workspace" : "Animation inspection workspace"}</p>
          </div>
        </div>
        <nav class="surface-switch" aria-label="Studio workspace">
          <button class={surface === "editor" ? "active" : ""} onClick={() => setSurface("editor")}>
            <span>01</span> Scene editor
          </button>
          <button class={surface === "viewer" ? "active" : ""} onClick={() => setSurface("viewer")}>
            <span>02</span> Animation viewer
          </button>
        </nav>
        {surface === "editor" ? (
          <div class="top-actions">
            <span class="status-copy">{notice}</span>
            <button class="icon-button" onClick={undo} disabled={!history.length} title="Undo (Ctrl+Z)">↶</button>
            <button class="icon-button" onClick={redo} disabled={!future.length} title="Redo (Ctrl+Shift+Z)">↷</button>
            <input
              ref={fileInput}
              class="visually-hidden"
              type="file"
              accept=".json,.osf.json"
              multiple
              onChange={(event) => void importFiles(event.currentTarget.files)}
            />
            <button class="secondary-button" onClick={() => fileInput.current?.click()}>Import JSON</button>
            <button
              class="primary-button"
              onClick={() => selectedDocument && download(selectedDocument.filename, serializeDocument(selectedDocument))}
              disabled={!selectedDocument}
            >
              Export JSON
            </button>
          </div>
        ) : (
          <div class="viewer-privacy"><i /> Local preview · files never uploaded</div>
        )}
      </header>

      {surface === "editor" ? <section class="workspace">
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
                    onClick={() => setSnapshot({
                      ...snapshot,
                      selection: { documentId: document.id, sceneIndex: 0 },
                    })}
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
                          onClick={() => setSnapshot({
                            ...snapshot,
                            selection: { documentId: document.id, sceneIndex: index },
                          })}
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
      </section> : <AnimationViewer />}
    </main>
  );
}
