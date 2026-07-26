import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import {
  ClipLibraryRepository,
  clipSetToOsfStage,
  exportLibraryManifest,
  resolveClipSet,
  type ClipLibrarySnapshot,
  type ClipRecord,
  type ClipSet,
} from "./clipLibrary";
import "./clipLibrary.css";

interface Props {
  onPreviewClip: (clipId: string) => void;
  onUseClip: (clip: ClipRecord) => void;
  onUseClipSet: (clipSet: ClipSet, clips: ClipRecord[]) => void;
}

const EMPTY_LIBRARY: ClipLibrarySnapshot = { clips: [], clipSets: [] };

function tagsFromInput(value: string): string[] {
  return value.split(",").map((tag) => tag.trim()).filter(Boolean);
}

function downloadBlob(blob: Blob, filename: string): void {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.click();
  queueMicrotask(() => URL.revokeObjectURL(url));
}

function formatBytes(bytes: number): string {
  if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KiB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MiB`;
}

export function ClipLibrary({ onPreviewClip, onUseClip, onUseClipSet }: Props) {
  const repository = useRef<ClipLibraryRepository>();
  const input = useRef<HTMLInputElement>(null);
  const [snapshot, setSnapshot] = useState(EMPTY_LIBRARY);
  const [thumbnailUrls, setThumbnailUrls] = useState<Record<string, string>>({});
  const snapshotRef = useRef(snapshot);
  const [mode, setMode] = useState<"clips" | "sets">("clips");
  const [query, setQuery] = useState("");
  const [selectedClipId, setSelectedClipId] = useState("");
  const [selectedSetId, setSelectedSetId] = useState("");
  const [notice, setNotice] = useState("Library assets stay on this device");
  const [busy, setBusy] = useState(true);

  const selectedClip = snapshot.clips.find((clip) => clip.id === selectedClipId);
  const selectedSet = snapshot.clipSets.find((set) => set.id === selectedSetId);
  const visibleClips = useMemo(() => {
    const needle = query.trim().toLowerCase();
    if (!needle) return snapshot.clips;
    return snapshot.clips.filter((clip) => [
      clip.title,
      clip.author,
      clip.sourceFilename,
      clip.gamePath,
      clip.format,
      ...clip.tags,
    ].some((value) => value.toLowerCase().includes(needle)));
  }, [snapshot.clips, query]);

  async function refreshThumbnails(next: ClipLibrarySnapshot): Promise<void> {
    if (!repository.current) return;
    const pairs = await Promise.all(next.clips.map(async (clip) => {
      const blob = await repository.current!.getThumbnail(clip.id);
      return blob ? [clip.id, URL.createObjectURL(blob)] as const : undefined;
    }));
    setThumbnailUrls((current) => {
      Object.values(current).forEach((url) => URL.revokeObjectURL(url));
      return Object.fromEntries(pairs.filter((pair): pair is readonly [string, string] => Boolean(pair)));
    });
  }

  function applySnapshot(update: (current: ClipLibrarySnapshot) => ClipLibrarySnapshot): void {
    const next = update(snapshotRef.current);
    snapshotRef.current = next;
    setSnapshot(next);
  }

  useEffect(() => {
    let active = true;
    void ClipLibraryRepository.open().then(async (nextRepository) => {
      if (!active) {
        nextRepository.close();
        return;
      }
      repository.current = nextRepository;
      const loaded = await nextRepository.load();
      if (!active) return;
      snapshotRef.current = loaded;
      setSnapshot(loaded);
      await refreshThumbnails(loaded);
      setSelectedClipId(loaded.clips[0]?.id ?? "");
      setSelectedSetId(loaded.clipSets[0]?.id ?? "");
      setBusy(false);
    }).catch((error) => {
      setNotice(error instanceof Error ? error.message : "The local library could not be opened.");
      setBusy(false);
    });
    return () => {
      active = false;
      repository.current?.close();
    };
  }, []);

  useEffect(() => () => {
    Object.values(thumbnailUrls).forEach((url) => URL.revokeObjectURL(url));
  }, [thumbnailUrls]);

  async function reload(preferredClipId = selectedClipId, preferredSetId = selectedSetId) {
    if (!repository.current) return;
    const loaded = await repository.current.load();
    snapshotRef.current = loaded;
    setSnapshot(loaded);
    await refreshThumbnails(loaded);
    setSelectedClipId(
      loaded.clips.some((clip) => clip.id === preferredClipId)
        ? preferredClipId
        : loaded.clips[0]?.id ?? "",
    );
    setSelectedSetId(
      loaded.clipSets.some((set) => set.id === preferredSetId)
        ? preferredSetId
        : loaded.clipSets[0]?.id ?? "",
    );
  }

  async function importFiles(files: FileList | null) {
    if (!files?.length || !repository.current) return;
    setBusy(true);
    const failures: string[] = [];
    let lastId = "";
    for (const file of Array.from(files)) {
      try {
        const record = await repository.current.importClip(file);
        if (record.format !== "af") {
          const recordId = record.id;
          void import("./clipInspection").then(async ({ inspectClip }) => {
            const inspection = await inspectClip(file);
            const inspectionRepository = await ClipLibraryRepository.open();
            let inspected: ClipRecord | null = null;
            try {
              await inspectionRepository.updateInspection(recordId, inspection);
              if (inspection.thumbnail) await inspectionRepository.saveThumbnail(recordId, inspection.thumbnail);
              inspected = await inspectionRepository.getClip(recordId);
            } finally {
              inspectionRepository.close();
            }
            if (inspected && repository.current) {
              applySnapshot((current) => ({
                ...current,
                clips: current.clips.map((clip) => clip.id === recordId ? inspected! : clip),
              }));
              await refreshThumbnails(snapshotRef.current);
            }
          }).catch(() => {
            // Import remains complete when optional metadata or WebGL preview generation fails.
          });
        }
        lastId = record.id;
      } catch (error) {
        failures.push(`${file.name}: ${error instanceof Error ? error.message : "Import failed"}`);
      }
    }
    await reload(lastId);
    setMode("clips");
    setNotice(failures.length
      ? failures.join(" · ")
      : `Imported ${files.length} clip${files.length === 1 ? "" : "s"}`);
    setBusy(false);
    if (input.current) input.current.value = "";
  }

  function patchClip(patch: Partial<ClipRecord>) {
    if (!selectedClip) return;
    applySnapshot((current) => ({
      ...current,
      clips: current.clips.map((clip) => clip.id === selectedClip.id ? { ...clip, ...patch } : clip),
    }));
  }

  async function saveClip() {
    const currentClip = snapshotRef.current.clips.find((clip) => clip.id === selectedClipId);
    if (!currentClip || !repository.current) return;
    const saved = await repository.current.updateClip(currentClip);
    await reload(saved.id);
    setNotice(`Saved metadata for ${saved.title}`);
  }

  async function removeClip() {
    if (!selectedClip || !repository.current) return;
    if (!window.confirm(`Remove "${selectedClip.title}" and its local animation bytes?`)) return;
    try {
      await repository.current.deleteClip(selectedClip.id);
      await reload("");
      setNotice("Removed clip from this device");
    } catch (error) {
      setNotice(error instanceof Error ? error.message : "The clip could not be removed.");
    }
  }

  function patchSet(patch: Partial<ClipSet>) {
    if (!selectedSet) return;
    applySnapshot((current) => ({
      ...current,
      clipSets: current.clipSets.map((set) => set.id === selectedSet.id ? { ...set, ...patch } : set),
    }));
  }

  function updateSetMembers(update: (members: ClipSet["members"]) => ClipSet["members"]) {
    if (!selectedSet) return;
    applySnapshot((current) => ({
      ...current,
      clipSets: current.clipSets.map((set) => set.id === selectedSet.id
        ? { ...set, members: update(set.members) }
        : set),
    }));
  }

  async function createSet() {
    if (!repository.current) return;
    const created = await repository.current.createClipSet();
    await reload(selectedClipId, created.id);
    setMode("sets");
    setNotice("Created a synchronized Clip Set");
  }

  async function saveSet() {
    const currentSet = snapshotRef.current.clipSets.find((set) => set.id === selectedSetId);
    if (!currentSet || !repository.current) return;
    const saved = await repository.current.updateClipSet(currentSet);
    await reload(selectedClipId, saved.id);
    setNotice(`Saved ${saved.title}`);
  }

  async function removeSet() {
    if (!selectedSet || !repository.current) return;
    if (!window.confirm(`Remove Clip Set "${selectedSet.title}"? The source clips will remain.`)) return;
    await repository.current.deleteClipSet(selectedSet.id);
    await reload(selectedClipId, "");
    setNotice("Removed Clip Set");
  }

  async function copyStage() {
    if (!selectedSet) return;
    try {
      const stage = clipSetToOsfStage(selectedSet, snapshot.clips);
      await navigator.clipboard.writeText(JSON.stringify(stage, null, 2));
      setNotice("Copied an OSF-compatible stage to the clipboard");
    } catch (error) {
      setNotice(error instanceof Error ? error.message : "The Clip Set could not be exported.");
    }
  }

  const dependencies = selectedSet ? resolveClipSet(selectedSet, snapshot.clips) : [];
  const missingDependencies = dependencies.filter((entry) => !entry.clip).length;
  const incompatibleSetMembers = dependencies.filter((entry) => entry.clip && entry.clip.actorCount !== 1).length;

  return (
    <section class="library-workspace">
      <header class="library-toolbar">
        <div>
          <span class="eyebrow">Local repository</span>
          <h2>Clip Library</h2>
          <p>{notice}</p>
        </div>
        <div class="library-actions">
          <input
            ref={input}
            class="visually-hidden"
            type="file"
            accept=".af,.glb,.gltf"
            multiple
            onChange={(event) => void importFiles(event.currentTarget.files)}
          />
          <button class="secondary-button" onClick={() => downloadBlob(exportLibraryManifest(snapshot), "osf-clip-library.json")}>
            Export catalog
          </button>
          <button class="primary-button" onClick={() => input.current?.click()} disabled={busy}>
            Import clips
          </button>
        </div>
      </header>

      <div class="library-tabs" role="tablist" aria-label="Library content">
        <button class={mode === "clips" ? "active" : ""} onClick={() => setMode("clips")}>
          Clips <span>{snapshot.clips.length}</span>
        </button>
        <button class={mode === "sets" ? "active" : ""} onClick={() => setMode("sets")}>
          Clip Sets <span>{snapshot.clipSets.length}</span>
        </button>
      </div>

      {mode === "clips" ? (
        <div class="library-layout">
          <div class="library-browser">
            <label class="library-search">
              <span>Search library</span>
              <input
                type="search"
                value={query}
                placeholder="Title, author, tag, path, format…"
                onInput={(event) => setQuery(event.currentTarget.value)}
              />
            </label>
            <div class="clip-grid">
              {visibleClips.map((clip) => (
                <button
                  key={clip.id}
                  class={`clip-tile ${clip.id === selectedClipId ? "selected" : ""}`}
                  onClick={() => setSelectedClipId(clip.id)}
                >
                  <div class={`clip-tile-preview ${thumbnailUrls[clip.id] ? "has-thumbnail" : ""}`}>
                    {thumbnailUrls[clip.id] && <img src={thumbnailUrls[clip.id]} alt="" />}
                    <span>{clip.format.toUpperCase()}</span>
                    <b>{clip.actorCount > 1 ? `${clip.actorCount} actors` : "solo"}</b>
                  </div>
                  <strong>{clip.title}</strong>
                  <small>{clip.author || "Unknown creator"}</small>
                  <div>
                    <span>{clip.durationSeconds ? `${clip.durationSeconds.toFixed(2)}s` : "Duration unset"}</span>
                    <span>{formatBytes(clip.size)}</span>
                  </div>
                </button>
              ))}
              {!visibleClips.length && (
                <div class="library-empty">
                  <span>◇</span>
                  <h3>{snapshot.clips.length ? "No matching clips" : "Your local clip shelf is empty"}</h3>
                  <p>{snapshot.clips.length
                    ? "Try another title, creator, tag, path, or format."
                    : "Import AF, GLB, or embedded glTF animations to start building scenes and synchronized Clip Sets."}</p>
                  {!snapshot.clips.length && <button class="primary-button" onClick={() => input.current?.click()}>Import first clips</button>}
                </div>
              )}
            </div>
          </div>

          <aside class="library-inspector">
            {selectedClip ? (
              <>
                <header>
                  <span class="eyebrow">Clip metadata</span>
                  <h2>{selectedClip.title}</h2>
                  <p>{selectedClip.sha256.slice(0, 12)} · {selectedClip.format.toUpperCase()}</p>
                </header>
                <div class="library-form">
                  <label class="field">
                    <span>Title</span>
                    <input value={selectedClip.title} onInput={(event) => patchClip({ title: event.currentTarget.value })} />
                  </label>
                  <label class="field">
                    <span>Creator</span>
                    <input value={selectedClip.author} placeholder="Community author" onInput={(event) => patchClip({ author: event.currentTarget.value })} />
                  </label>
                  <label class="field">
                    <span>Game-relative file path</span>
                    <input value={selectedClip.gamePath} onInput={(event) => patchClip({ gamePath: event.currentTarget.value })} />
                  </label>
                  <div class="field-grid two">
                    <label class="field">
                      <span>Actors</span>
                      <input type="number" min="1" value={selectedClip.actorCount} onInput={(event) => patchClip({ actorCount: Number(event.currentTarget.value) })} />
                    </label>
                    <label class="field">
                      <span>Duration <small>seconds</small></span>
                      <input
                        type="number"
                        min="0"
                        step="0.01"
                        value={selectedClip.durationSeconds ?? ""}
                        onInput={(event) => patchClip({ durationSeconds: Number(event.currentTarget.value) || undefined })}
                      />
                    </label>
                  </div>
                  <label class="field">
                    <span>Rig identifier</span>
                    <input value={selectedClip.rigId ?? ""} placeholder="human, bipeda…" onInput={(event) => patchClip({ rigId: event.currentTarget.value || undefined })} />
                  </label>
                  <label class="field">
                    <span>Tags <small>comma separated</small></span>
                    <input value={selectedClip.tags.join(", ")} onInput={(event) => patchClip({ tags: tagsFromInput(event.currentTarget.value) })} />
                  </label>
                  <dl class="clip-facts">
                    <div><dt>Source</dt><dd>{selectedClip.sourceFilename}</dd></div>
                    <div><dt>Size</dt><dd>{formatBytes(selectedClip.size)}</dd></div>
                    <div><dt>Animations</dt><dd>{selectedClip.animationCount ?? "Unknown"}</dd></div>
                    <div><dt>Bones</dt><dd>{selectedClip.boneCount ?? "Unknown"}</dd></div>
                    <div><dt>Storage</dt><dd>Browser-local</dd></div>
                  </dl>
                </div>
                <footer class="library-inspector-actions">
                  <button class="secondary-button" onClick={() => onPreviewClip(selectedClip.id)}>Open in viewer</button>
                  <button class="secondary-button" disabled={selectedClip.actorCount !== 1} title={selectedClip.actorCount !== 1 ? "Use single-actor clips in scene slots; group them with a Clip Set." : ""} onClick={() => onUseClip(selectedClip)}>Use in scene</button>
                  <button class="primary-button" onClick={() => void saveClip()}>Save metadata</button>
                  <button class="danger-quiet" onClick={() => void removeClip()}>Remove clip</button>
                </footer>
              </>
            ) : (
              <div class="library-empty compact"><p>Select a clip to inspect its metadata.</p></div>
            )}
          </aside>
        </div>
      ) : (
        <div class="library-layout">
          <aside class="clipset-list">
            <header>
              <div><span class="eyebrow">Synchronized groups</span><h3>Clip Sets</h3></div>
              <button class="add-button" aria-label="Create Clip Set" onClick={() => void createSet()}>+</button>
            </header>
            {snapshot.clipSets.map((set) => (
              <button class={set.id === selectedSetId ? "selected" : ""} onClick={() => setSelectedSetId(set.id)}>
                <strong>{set.title}</strong>
                <span>{set.members.length} role{set.members.length === 1 ? "" : "s"}</span>
              </button>
            ))}
            {!snapshot.clipSets.length && <p>Create a Clip Set to coordinate one animation per actor.</p>}
          </aside>

          <div class="clipset-editor">
            {selectedSet ? (
              <>
                <header class="clipset-heading">
                  <div>
                    <span class="eyebrow">Multi-actor composition</span>
                    <h2>{selectedSet.title}</h2>
                    <p>Each member resolves to a stable local clip ID and exports as a normal OSF stage.</p>
                  </div>
                  <div class={`dependency-health ${missingDependencies || incompatibleSetMembers ? "error" : ""}`}>
                    {missingDependencies ? `${missingDependencies} missing` : incompatibleSetMembers ? `${incompatibleSetMembers} incompatible` : "Dependencies ready"}
                  </div>
                </header>
                <section class="panel clipset-details">
                  <div class="field-grid two">
                    <label class="field"><span>Title</span><input value={selectedSet.title} onInput={(event) => patchSet({ title: event.currentTarget.value })} /></label>
                    <label class="field"><span>Creator</span><input value={selectedSet.author} onInput={(event) => patchSet({ author: event.currentTarget.value })} /></label>
                    <label class="field span-two"><span>Tags <small>comma separated</small></span><input value={selectedSet.tags.join(", ")} onInput={(event) => patchSet({ tags: tagsFromInput(event.currentTarget.value) })} /></label>
                  </div>
                </section>
                <section class="panel">
                  <div class="section-title">
                    <div><span class="eyebrow">Cast mapping</span><h3>Actor clips</h3></div>
                    <button
                      class="secondary-button"
                      disabled={!snapshot.clips.length}
                      onClick={() => updateSetMembers((members) => [...members, {
                          id: crypto.randomUUID(),
                          role: `actor${selectedSet.members.length + 1}`,
                          clipId: snapshot.clips[0]?.id ?? "",
                          offset: { x: 0, y: 0, z: 0, heading: 0 },
                        }])}
                    >Add actor</button>
                  </div>
                  <div class="clipset-members">
                    {selectedSet.members.map((member, index) => {
                      const dependency = dependencies[index];
                      return (
                        <div class={`clipset-member ${dependency?.clip ? "" : "missing"}`}>
                          <span class="card-index">{String(index + 1).padStart(2, "0")}</span>
                          <label class="field"><span>Role</span><input value={member.role} onInput={(event) => updateSetMembers((members) => members.map((item) => item.id === member.id ? { ...item, role: event.currentTarget.value } : item))} /></label>
                          <label class="field">
                            <span>Clip</span>
                            <select value={member.clipId} onChange={(event) => updateSetMembers((members) => members.map((item) => item.id === member.id ? { ...item, clipId: event.currentTarget.value } : item))}>
                              {!dependency?.clip && <option value={member.clipId}>Missing clip</option>}
                              {snapshot.clips.map((clip) => <option value={clip.id}>{clip.title}</option>)}
                            </select>
                          </label>
                          <div class="clipset-placement">
                            {(["x", "y", "z", "heading"] as const).map((axis) => (
                              <label class="field compact-field">
                                <span>{axis}</span>
                                <input
                                  type="number"
                                  step={axis === "heading" ? 1 : 0.1}
                                  value={member.offset[axis]}
                                  onInput={(event) => updateSetMembers((members) => members.map((item) => item.id === member.id
                                    ? { ...item, offset: { ...item.offset, [axis]: Number(event.currentTarget.value) } }
                                    : item))}
                                />
                              </label>
                            ))}
                          </div>
                          <button class="remove-button" aria-label={`Remove ${member.role}`} onClick={() => updateSetMembers((members) => members.filter((item) => item.id !== member.id))}>×</button>
                        </div>
                      );
                    })}
                    {!selectedSet.members.length && <div class="library-empty compact"><p>Add an actor for each synchronized animation in this scene segment.</p></div>}
                  </div>
                </section>
                <div class="clipset-actions">
                  <button class="danger-quiet" onClick={() => void removeSet()}>Remove Clip Set</button>
                  <button class="secondary-button" disabled={!selectedSet.members.length || Boolean(missingDependencies) || Boolean(incompatibleSetMembers)} onClick={() => void copyStage()}>Copy OSF stage</button>
                  <button class="secondary-button" disabled={!selectedSet.members.length || Boolean(missingDependencies) || Boolean(incompatibleSetMembers)} onClick={() => onUseClipSet(selectedSet, snapshot.clips)}>Use in scene</button>
                  <button class="primary-button" onClick={() => void saveSet()}>Save Clip Set</button>
                </div>
              </>
            ) : (
              <div class="library-empty">
                <span>◇◇</span>
                <h3>Compose synchronized actor clips</h3>
                <p>A Clip Set connects one raw animation to each scene role while keeping every source clip independently reusable.</p>
                <button class="primary-button" onClick={() => void createSet()}>Create Clip Set</button>
              </div>
            )}
          </div>
        </div>
      )}
    </section>
  );
}
