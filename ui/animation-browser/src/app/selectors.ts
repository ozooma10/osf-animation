import { evaluateScene, type ImportFile, type SceneEvaluation, type SceneModel, type SceneStage } from "../model";
import { PLAYER_TOKEN, type ActiveScene, type BrowserState, type CastMember, type FurnitureTarget, type PickTarget } from "./state";

/** The pick target under the pointer. THE one scoring function for world picking:
 *  the marker layer uses it to light the hot marker and the click handler uses it
 *  to resolve the selection, so the two can never disagree. Among the targets whose
 *  acceptance ellipse contains the pointer, the FRONT-MOST wins (what a ray into the
 *  scene would hit first — the target the user visually sees); targets at effectively
 *  the same depth (within 5% — depth's absolute unit is native-defined, so only
 *  relative comparisons are safe) fall back to the better ellipse score.
 *  Pointer is in CSS pixels. */
export function hottestPickTarget(
  targets: readonly PickTarget[], px: number, py: number, width: number, height: number,
): PickTarget | null {
  let best: PickTarget | null = null;
  let bestScore = 1;
  for (const target of targets) {
    const dx = (px - target.cx * width) / target.rx;
    const dy = (py - target.cy * height) / target.ry;
    const score = dx * dx + dy * dy;
    if (score > 1) continue;
    const band = best ? 0.05 * Math.min(target.depth, best.depth) : 0;
    if (!best || target.depth < best.depth - band
      || (Math.abs(target.depth - best.depth) <= band && score < bestScore)) {
      best = target;
      bestScore = score;
    }
  }
  return best;
}

export const WHEEL_MAX = 12;

export interface LocationCastChoices {
  showPlayer: boolean;
  actors: CastMember[];
}

export function locationCastChoices(state: BrowserState): LocationCastChoices {
  const castAToken = state.cast[0]?.token;
  const seen = new Set<number>([PLAYER_TOKEN]);
  if (castAToken != null) seen.add(castAToken);

  const actors = state.cast.filter((member) => {
    if (seen.has(member.token)) return false;
    seen.add(member.token);
    return true;
  });
  return { showPlayer: castAToken !== PLAYER_TOKEN, actors };
}

export interface WheelCandidate {
  key: string;
  scene: string;
  stage: number | null;
  title: string;
  detail: string;
  pinned: number;
  priority: number;
  weight: number;
  source: SceneModel | SceneStage;
}

export type PlayableKind = "animation" | "action" | "scene";

/**
 * Browser-facing launch target. A collection is deliberately absent: packs,
 * folders, and vanilla sets organize these items but are never launchable.
 */
export interface PlayableItem {
  key: string;
  kind: PlayableKind;
  scene: SceneModel;
  stage: SceneStage | null;
  title: string;
  collection: string;
}

export function isEmote(scene: SceneModel | null | undefined): boolean {
  return !!scene && scene.tags.some((tag) => tag.toLowerCase().startsWith("player.emote."));
}

export function isWheelEmote(scene: SceneModel | null | undefined): boolean {
  return !!scene && isEmote(scene) && !scene.unlisted && scene.actorCount === 1 && !scene.requiresFurniture;
}

export function isWheelStage(scene: SceneModel, stage: SceneStage): boolean {
  return stage.clipCount > 0 && !!scene.library && !scene.requiresFurniture
    && scene.actorCount === 1 && scene.species === "human";
}

export function sceneCatalog(state: BrowserState): SceneModel[] {
  return state.catalog.filter((scene) => !isEmote(scene) && unlistedVisible(state, scene));
}

export function emoteCatalog(state: BrowserState): SceneModel[] {
  return state.catalog.filter((scene) => isEmote(scene) && unlistedVisible(state, scene));
}

/** A clip entry the engine HARVESTED from a scene's stages — the author-only debug surface that
 *  lets a multi-actor scene be inspected one raw clip at a time. Entries a pack REGISTERED via
 *  `clipLibrary` share the same id namespace but are authored content whose whole purpose is to
 *  appear under Animations, so `curated` excludes them: without that check a pack shipping only a
 *  clip library (no scenes) was invisible unless the user revealed hidden content. */
export function isGeneratedSceneClip(scene: SceneModel): boolean {
  return !scene.curated && scene.id.toLowerCase().startsWith("osf.scene-clip/");
}

export function playableKey(sceneId: string, stage: number | null): string {
  return wheelKey(sceneId, stage);
}

/**
 * Turn engine/export names into labels without changing registry identity.
 * Authored prose is left alone; separators, CamelCase, and numeric suffixes
 * used by vanilla clip names are made readable.
 */
export function readableAnimationName(value: string, pose = false): string {
  let label = value.trim()
    .replace(/^LooseAnim[_\s-]*/i, "")
    .replace(/_/g, " ")
    .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
    .replace(/([A-Z])([A-Z][a-z])/g, "$1 $2")
    .replace(/([A-Za-z])(\d)/g, "$1 $2")
    .replace(/(\d)([A-Za-z])/g, "$1 $2")
    .replace(/\s+/g, " ")
    .trim();
  if (pose) label = label.replace(/\s+Pose$/i, "").trim();
  return label || value;
}

function readableCollectionName(value: string): string {
  return value.split(/(\s+[·—]\s+|\s*\/\s*)/).map((part) =>
    /^[\s/·—]+$/.test(part) ? part : readableAnimationName(part)).join("");
}

export function playableSceneTitle(scene: SceneModel): string {
  return readableCollectionName(scene.title);
}

export function playableStageTitle(scene: SceneModel, stage: SceneStage): string {
  const pose = stage.tags.some((tag) => tag.toLowerCase() === "pose");
  if (scene.stages.length === 1 && !stage.name) {
    return readableCollectionName(scene.title.replace(/^Vanilla · /, ""));
  }
  return stage.name ? readableAnimationName(stage.name, pose) : `Animation ${stage.index + 1}`;
}

// Single-entry memo: the full playable list is ~6k items over the shipped fixtures and was
// measured at ~23 ms per rebuild — and it used to rebuild at least twice per render while the
// 12.5 Hz projectActors poll forced renders. The result only depends on the three inputs below,
// all of which change by reference/value, so one cached entry suffices.
let playableMemo: {
  catalog: BrowserState["catalog"];
  library: BrowserState["library"];
  libCustomOnly: boolean;
  items: PlayableItem[];
} | null = null;

export function playableItems(state: BrowserState): PlayableItem[] {
  if (playableMemo &&
    playableMemo.catalog === state.catalog &&
    playableMemo.library === state.library &&
    playableMemo.libCustomOnly === state.libCustomOnly) {
    return playableMemo.items;
  }
  const items = buildPlayableItems(state);
  playableMemo = {
    catalog: state.catalog,
    library: state.library,
    libCustomOnly: state.libCustomOnly,
    items,
  };
  return items;
}

function buildPlayableItems(state: BrowserState): PlayableItem[] {
  const items: PlayableItem[] = [];
  for (const scene of state.catalog) {
    const kind: PlayableKind = isEmote(scene) ? "action" : "scene";
    items.push({
      key: playableKey(scene.id, null),
      kind,
      scene,
      stage: null,
      title: playableSceneTitle(scene),
      collection: scene.folder,
    });
  }
  for (const scene of filteredLibrary(state)) {
    for (const stage of scene.stages) {
      items.push({
        key: playableKey(scene.id, stage.index),
        kind: "animation",
        scene,
        stage,
        title: playableStageTitle(scene, stage),
        collection: [scene.folder, scene.stages.length > 1
          ? readableCollectionName(scene.title.replace(/^Vanilla · /, ""))
          : ""]
          .filter(Boolean).join(" / "),
      });
    }
  }
  return items;
}

function playablePreference(item: PlayableItem): number {
  if (item.kind === "animation" && item.stage) {
    const pose = item.stage.tags.some((tag) => tag.toLowerCase() === "pose");
    const photomode = /(^|\/)photomode(\/|$)/i.test(item.scene.id)
      || item.scene.tags.some((tag) => tag.toLowerCase() === "photomode");
    if (pose && photomode) return 0;
    if (pose) return 1;
    return 2;
  }
  if (item.kind === "scene") return 3;
  return 4;
}

/** Shared ordering for rendering and automatic selection. */
export function comparePlayableItems(state: BrowserState, a: PlayableItem, b: PlayableItem): number {
  const anchored = (item: PlayableItem) => Number(
    !!state.furniture && item.scene.requiresFurniture && fitsKeyedAnchor(state, item.scene) === true);
  return anchored(b) - anchored(a)
    || playablePreference(a) - playablePreference(b)
    || b.scene.priority - a.scene.priority
    || a.title.localeCompare(b.title)
    || a.key.localeCompare(b.key);
}

export function playableGroupOpen(state: BrowserState, key: string, containsSelection: boolean): boolean {
  if (state.filters.search) return true;
  return state.libOpen.get(key) ?? containsSelection;
}

export function matchesPlayableSearch(state: BrowserState, item: PlayableItem): boolean {
  if (!state.filters.search) return true;
  const scene = item.scene;
  const roles = scene.roles.map((role) => `${role.name} ${role.gender}`).join(" ");
  return `${item.title} ${item.collection} ${scene.title} ${scene.id} ${scene.tags.join(" ")} ${item.stage?.tags.join(" ") ?? ""} ${roles} ${scene.pack} ${scene.sourceFile} ${scene.sourcePath}`
    .toLowerCase().includes(state.filters.search);
}

export function playableVisible(state: BrowserState, item: PlayableItem): boolean {
  if (isHiddenPlayable(item) && !state.showHidden) return false;
  if (state.browseKind !== "all" && state.browseKind !== item.kind) return false;
  if (!speciesVisible(state, item.scene) || !matchesPlayableSearch(state, item)) return false;
  if (item.kind === "animation") {
    const matchKnown = !!state.furniture && state.anchorMatch?.token === state.furniture.token;
    if (matchKnown && !state.anchorMatch?.ids.has(item.scene.id)) return false;
    if (!matchKnown && !state.libFull && !state.filters.search && item.stage && !stageClean(item.stage)) return false;
  }
  return true;
}

export function isHiddenPlayable(item: PlayableItem): boolean {
  return item.kind === "animation"
    ? isGeneratedSceneClip(item.scene)
    : !item.scene.library && item.scene.unlisted;
}

/** Distinct hidden scene records that the current Browse filters would reveal. */
export function hiddenSceneCount(state: BrowserState): number {
  const revealed = state.showHidden ? state : { ...state, showHidden: true };
  return new Set(playableItems(state)
    .filter((item) => isHiddenPlayable(item) && playableVisible(revealed, item))
    .map((item) => item.scene.id)).size;
}

export function selectedPlayable(state: BrowserState): PlayableItem | null {
  if (!state.selectedId) return null;
  const items = playableItems(state);
  const exact = items.find((item) => item.scene.id === state.selectedId
    && (item.stage?.index ?? null) === state.selectedStage);
  return exact ?? items.find((item) => item.scene.id === state.selectedId) ?? null;
}

export function animationList(state: BrowserState): SceneModel[] {
  return [
    ...emoteCatalog(state),
    ...filteredLibrary(state).filter((scene) => state.showHidden || !isGeneratedSceneClip(scene)),
  ];
}

export function isVanillaAnimation(scene: SceneModel): boolean {
  return scene.id.toLowerCase().startsWith("vanilla/")
    || scene.tags.some((tag) => tag.toLowerCase() === "vanilla");
}

export function filteredLibrary(state: BrowserState): SceneModel[] {
  return state.libCustomOnly
    ? state.library.filter((scene) => !isVanillaAnimation(scene))
    : state.library;
}

export function sceneById(state: BrowserState, id: string | null): SceneModel | null {
  if (!id) return null;
  return state.catalog.find((scene) => scene.id === id)
    ?? state.library.find((scene) => scene.id === id)
    ?? null;
}

export function sceneTitle(state: BrowserState, id: string): string {
  const scene = sceneById(state, id);
  return scene ? playableSceneTitle(scene) : id || "scene";
}

export function activeScenes(state: BrowserState): ActiveScene[] {
  if (state.active) return state.active;
  return state.lastHandle
    ? [{ handle: state.lastHandle, sceneId: state.lastSceneId, stage: 0, player: true, cast: [], time: 0, duration: 0, speed: 0, inspection: false }]
    : [];
}

export function busyTokens(state: BrowserState): ReadonlySet<number> {
  const tokens = new Set<number>();
  for (const scene of activeScenes(state)) {
    for (const member of scene.cast) tokens.add(member.token);
  }
  return tokens;
}

export function hasPlayer(state: BrowserState): boolean {
  return state.cast.some((member) => member.kind === "player");
}

/**
 * Cast members whose world labels are live right now, paired with their cast
 * index (the A/B/C key). Empty whenever nothing would render them — the same
 * gate decides whether the controller runs the 80ms projection poll at all.
 */
export function labeledCast(state: BrowserState): { member: CastMember; index: number }[] {
  if (!state.preferences.actorLabels || !state.viewVisible || state.wheel || state.minimized) return [];
  return state.cast
    .map((member, index) => ({ member, index }))
    .filter(({ member }) => member.kind !== "player");
}

/** Selected furniture whose world label is live under the same display policy
 *  as selected-cast labels. */
export function labeledFurniture(state: BrowserState): FurnitureTarget | null {
  if (!state.preferences.actorLabels || !state.viewVisible || state.wheel || state.minimized) return null;
  return state.furniture;
}

export function unlistedVisible(state: BrowserState, scene: SceneModel): boolean {
  return !!scene.library || !scene.unlisted || state.showHidden;
}

export function evaluateForState(state: BrowserState, scene: SceneModel): SceneEvaluation {
  return evaluateScene(scene, {
    castCount: state.cast.length,
    furnitureToken: state.furniture?.token ?? null,
    anchorMatch: state.anchorMatch,
  });
}

export function castSpecies(state: BrowserState): ReadonlySet<string> {
  return new Set(state.cast.map((member) => member.kind === "player" ? "human" : member.species || "human"));
}

export function castHasCreature(state: BrowserState): boolean {
  return state.cast.some((member) => (member.kind === "player" ? "human" : member.species || "human") !== "human");
}

export function speciesLabel(species: string): string {
  if (!species || species === "human") return "Human";
  return species.replace(/([a-z])([A-Z])/g, "$1 $2").replace(/^./, (character) => character.toUpperCase());
}

export function speciesVisible(state: BrowserState, scene: SceneModel): boolean {
  if (state.allSpecies) return true;
  const species = castSpecies(state);
  return species.size === 0 || species.has(scene.species || "human");
}

export function matchesSearch(state: BrowserState, scene: SceneModel): boolean {
  if (!unlistedVisible(state, scene)) return false;
  if (!state.filters.search) return true;
  const roles = scene.roles.map((role) => `${role.name} ${role.gender}`).join(" ");
  const stages = scene.library ? ` ${scene.stageHay ?? ""}` : "";
  return `${scene.title} ${scene.id} ${scene.tags.join(" ")} ${roles} ${scene.pack} ${scene.folder} ${scene.sourceFile} ${scene.sourcePath}${stages}`
    .toLowerCase()
    .includes(state.filters.search);
}

// The only caller passes mode === "active" (validSelection), so this is just the search +
// species filter — the old browseVisible carried unreachable "scenes"/"library" branches.
export function browseVisible(state: BrowserState, scene: SceneModel): boolean {
  return matchesSearch(state, scene) && speciesVisible(state, scene);
}

/** The unavailable-scenes preference resolved against the reveal toggle (the UnifiedBrowser split). */
export function showUnavailable(state: BrowserState): boolean {
  return state.preferences.unavailableScenes === "show"
    || state.preferences.unavailableScenes === "ask" && state.browseAll;
}

export function selectionCandidates(state: BrowserState): SceneModel[] {
  if (state.mode === "active") return state.catalog;
  const seen = new Set<string>();
  return playableItems(state).filter((item) => playableVisible(state, item))
    .map((item) => item.scene)
    .filter((scene) => !seen.has(scene.id) && !!seen.add(scene.id));
}

export function validSelection(state: BrowserState): string | null {
  if (state.mode === "active" && state.selectedId && sceneById(state, state.selectedId)) return state.selectedId;
  const candidates = selectionCandidates(state);
  const visible = state.mode === "active" ? candidates.filter((scene) => browseVisible(state, scene)) : candidates;
  if (state.selectedId && visible.some((scene) => scene.id === state.selectedId)) return state.selectedId;
  if (visible[0]) return visible[0].id;
  if (state.selectedId && candidates.some((scene) => scene.id === state.selectedId)) return state.selectedId;
  return candidates.find((scene) => unlistedVisible(state, scene))?.id ?? candidates[0]?.id ?? null;
}

export function anchorShort(scene: SceneModel): string {
  return scene.anchors.length > 1 ? `${scene.anchors[0]} +${scene.anchors.length - 1}` : scene.anchors[0] ?? "";
}

export function anchorFull(scene: SceneModel): string {
  return scene.anchors.join(" / ");
}

export function needsText(state: BrowserState, scene: SceneModel, evaluation = evaluateForState(state, scene)): string {
  if (!evaluation.rolesGate) {
    const count = evaluation.actorCount - evaluation.castCount;
    return `+${count} actor${count === 1 ? "" : "s"}`;
  }
  if (!evaluation.anchorGate) {
    const anchor = anchorShort(scene);
    return anchor ? `needs ${anchor}` : state.furniture ? "other furniture" : "needs furniture";
  }
  if (evaluation.overCast) {
    const count = evaluation.castCount - evaluation.actorCount;
    return `-${count} crew`;
  }
  return "";
}

export function packKey(scene: SceneModel): string {
  if (scene.pack) return `pack:${scene.pack.toLowerCase()}`;
  const file = scene.sourceFile.replace(/\\/g, "/").split("/").pop() ?? "";
  if (file) return file.replace(/\.osf\.json$/i, "");
  const segments = scene.id.split("/").filter(Boolean);
  return segments.slice(0, Math.max(1, Math.min(2, segments.length))).join("-") || "library";
}

export function packLabel(key: string, scenes: readonly SceneModel[]): string {
  return scenes[0]?.pack
    ? scenes[0].pack.toUpperCase()
    : key.replace(/^vanilla-/i, "").replace(/[-_]+/g, " ").toUpperCase();
}

/**
 * Keep the immediately useful vanilla pose catalog prominent and the raw
 * quest/dialogue Scene fragments out of the way. Returning zero for every
 * other pairing preserves the item-ranked insertion order between packs.
 */
export function comparePlayableGroupKeys(a: string, b: string): number {
  const rank = (key: string) => {
    if (/^vanilla-photomode$/i.test(key)) return -1;
    if (/^vanilla-scenes$/i.test(key)) return 1;
    return 0;
  };
  return rank(a) - rank(b);
}

export function fitsKeyedAnchor(state: BrowserState, scene: SceneModel): boolean | null {
  if (!scene.requiresFurniture) return null;
  if (!state.furniture || state.anchorMatch?.token !== state.furniture.token) return null;
  return state.anchorMatch.ids.has(scene.id);
}

export function stageClean(stage: SceneStage): boolean {
  return !stage.tags.includes("transition") && !stage.tags.includes("partial");
}

export function wheelKey(scene: string, stage: number | null): string {
  return `${scene}\0${stage == null ? "" : stage}`;
}

export function wheelStageTitle(scene: SceneModel, stage: SceneStage): string {
  if (stage.name) return playableStageTitle(scene, stage);
  const title = playableSceneTitle(scene);
  return scene.stages.length === 1 ? title : `${title} · Stage ${stage.index + 1}`;
}

// Same single-entry memo shape as playableItems: the candidate sweep walks every library stage.
let wheelMemo: {
  catalog: BrowserState["catalog"];
  library: BrowserState["library"];
  candidates: WheelCandidate[];
} | null = null;

export function wheelCandidates(state: BrowserState): WheelCandidate[] {
  if (wheelMemo && wheelMemo.catalog === state.catalog && wheelMemo.library === state.library) {
    return wheelMemo.candidates;
  }
  const candidates = buildWheelCandidates(state);
  wheelMemo = { catalog: state.catalog, library: state.library, candidates };
  return candidates;
}

function buildWheelCandidates(state: BrowserState): WheelCandidate[] {
  const candidates: WheelCandidate[] = emoteCatalog(state).filter(isWheelEmote).map((scene) => ({
    key: wheelKey(scene.id, null),
    scene: scene.id,
    stage: null,
    title: playableSceneTitle(scene),
    detail: playableSceneTitle(scene),
    pinned: scene.pinned,
    priority: scene.priority,
    weight: scene.weight,
    source: scene,
  }));
  for (const scene of state.library) {
    for (const stage of scene.stages) {
      if (!isWheelStage(scene, stage)) continue;
      const title = wheelStageTitle(scene, stage);
      candidates.push({
        key: wheelKey(scene.id, stage.index),
        scene: scene.id,
        stage: stage.index,
        title,
        detail: stage.name ? `${playableSceneTitle(scene)} · ${playableStageTitle(scene, stage)}` : title,
        pinned: stage.pinned,
        priority: scene.priority,
        weight: scene.weight,
        source: stage,
      });
    }
  }
  return candidates;
}

export function wheelPool(state: BrowserState): WheelCandidate[] {
  const eligible = wheelCandidates(state);
  const pinned = eligible.filter((item) => item.pinned > 0).sort((a, b) => a.pinned - b.pinned);
  if (state.wheelCustomized) return pinned.slice(0, WHEEL_MAX);
  const prefix = state.wheel?.tagPrefix || "player.emote.";
  return eligible
    .filter((item) => item.stage == null && "tags" in item.source
      && item.source.tags.some((tag) => tag.toLowerCase().startsWith(prefix)))
    .sort((a, b) => b.priority - a.priority || b.weight - a.weight || a.title.localeCompare(b.title))
    .slice(0, WHEEL_MAX);
}

export function formatDuration(seconds: number | null): string {
  if (seconds == null || !Number.isFinite(seconds) || seconds < 0) return "";
  const rounded = Math.max(1, Math.round(seconds));
  return rounded < 60 ? `${rounded}s` : `${Math.floor(rounded / 60)}:${String(rounded % 60).padStart(2, "0")}`;
}

export function formatEstimate(scene: Pick<SceneModel, "estSec" | "estPartial" | "openEnded">): string {
  const duration = formatDuration(scene.estSec);
  if (!duration) return scene.openEnded ? "∞" : "";
  return `~${duration}${scene.estPartial ? "+" : ""}${scene.openEnded ? "∞" : ""}`;
}

export function stageLabel(scene: SceneModel, index: number): string {
  const stage = scene.stages.find((candidate) => candidate.index === index);
  return stage ? playableStageTitle(scene, stage) : `stage ${index}`;
}

// ---- import report ----------------------------------------------------------------------------

/** Worst state a file reached: an error outranks a warning, which outranks a silent oddity. */
export type ImportSeverity = "ok" | "note" | "warn" | "error";
export type ImportOutcome = "clean" | "empty" | "missing" | "partial" | "rejected";

export function importOutcome(file: ImportFile): ImportOutcome {
  if (file.rejected || (!file.scenes && !file.clipEntries && file.errors > 0)) return "rejected";
  if (file.errors || file.warnings) return "partial";
  if (file.missingClips || file.hidden) return "missing";
  if (!file.scenes && !file.clipEntries) return "empty";
  return "clean";
}

export function importSeverity(file: ImportFile): ImportSeverity {
  const outcome = importOutcome(file);
  if (outcome === "rejected" || outcome === "partial" && file.errors > 0) return "error";
  if (outcome === "partial" || outcome === "missing") return "warn";
  return outcome === "empty" ? "note" : "ok";
}

export function importResult(file: ImportFile): string {
  const outcome = importOutcome(file);
  if (outcome === "rejected") {
    return file.declaredScenes
      ? `Rejected all ${file.declaredScenes} authored scene${file.declaredScenes === 1 ? "" : "s"}; fix the errors and reload.`
      : "Rejected before it could contribute any content.";
  }
  if (outcome === "partial") {
    if (file.rejectedScenes) return `${file.scenes} of ${file.declaredScenes} scenes loaded; ${file.rejectedScenes} rejected.`;
    return `Loaded ${file.scenes} scene${file.scenes === 1 ? "" : "s"} with ${file.errors + file.warnings} diagnostic${file.errors + file.warnings === 1 ? "" : "s"}.`;
  }
  if (outcome === "missing") {
    if (file.hidden) return `${file.hidden} scene${file.hidden === 1 ? "" : "s"} unavailable because ${file.missingClips} clip${file.missingClips === 1 ? " is" : "s are"} missing.`;
    return `${file.missingClips} referenced clip${file.missingClips === 1 ? " is" : "s are"} missing; loaded content may still be incomplete.`;
  }
  if (outcome === "empty") return "Loaded successfully, but contributed no scenes or clip entries.";
  const parts: string[] = [];
  if (file.scenes) parts.push(`${file.scenes} scene${file.scenes === 1 ? "" : "s"}`);
  if (file.clipEntries) parts.push(`${file.clipEntries} clip entr${file.clipEntries === 1 ? "y" : "ies"}`);
  return `Loaded ${parts.join(" and ")} cleanly.`;
}

export interface ImportOutcomeCounts {
  all: number;
  attention: number;
  clean: number;
  empty: number;
  missing: number;
  partial: number;
  rejected: number;
}

export function importOutcomeCounts(files: readonly ImportFile[]): ImportOutcomeCounts {
  const counts: ImportOutcomeCounts = { all: files.length, attention: 0, clean: 0, empty: 0, missing: 0, partial: 0, rejected: 0 };
  for (const file of files) {
    const outcome = importOutcome(file);
    counts[outcome]++;
    if (outcome !== "clean") counts.attention++;
  }
  return counts;
}

const SEVERITY_RANK: Record<ImportSeverity, number> = { error: 0, warn: 1, note: 2, ok: 3 };

function importMatchesFilter(state: BrowserState, file: ImportFile): boolean {
  const outcome = importOutcome(file);
  return state.importsFilter === "all"
    || state.importsFilter === "attention" && outcome !== "clean"
    || state.importsFilter === outcome;
}

export function visibleImports(state: BrowserState): ImportFile[] {
  const search = state.importsSearch;
  return state.imports
    .filter((file) => {
      if (!importMatchesFilter(state, file)) return false;
      if (!search) return true;
      const diagnostics = file.problems.map((problem) =>
        `${problem.code} ${problem.message} ${problem.hint} ${problem.scene} ${problem.node} ${problem.role} ${problem.clip}`).join(" ");
      return `${file.path} ${file.file} ${file.pack} ${file.species.join(" ")} ${file.missingClipExamples.join(" ")} ${diagnostics}`
        .toLowerCase().includes(search);
    })
    .sort((a, b) => SEVERITY_RANK[importSeverity(a)] - SEVERITY_RANK[importSeverity(b)]
      || a.path.localeCompare(b.path));
}

export interface ImportGroup {
  key: string;
  label: string;
  files: ImportFile[];
  severity: ImportSeverity;
  problems: number;
}

export function importGroups(state: BrowserState): ImportGroup[] {
  const groups = new Map<string, ImportGroup>();
  for (const file of visibleImports(state)) {
    const folder = file.path.split("/").filter(Boolean)[0] || "";
    const key = !file.path ? "cross-file" : file.pack ? `pack:${file.pack.toLowerCase()}` : `folder:${folder.toLowerCase() || "unlabeled"}`;
    const label = !file.path ? "Registry-wide" : file.pack || (folder ? `${folder} folder` : "Unlabeled files");
    const existing = groups.get(key);
    if (existing) {
      existing.files.push(file);
      existing.problems += file.errors + file.warnings;
      if (SEVERITY_RANK[importSeverity(file)] < SEVERITY_RANK[existing.severity]) existing.severity = importSeverity(file);
    } else {
      groups.set(key, { key, label, files: [file], severity: importSeverity(file), problems: file.errors + file.warnings });
    }
  }
  return [...groups.values()];

}
export function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(bytes < 10 * 1024 ? 1 : 0)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export function formatMillis(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return "0 ms";
  return ms < 10 ? `${ms.toFixed(1)} ms` : ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(2)} s`;
}

export function wheelGeometry(count: number): { rx: number; ry: number } {
  const scale = Math.max(0, Math.min(1, (count - 3) / 9));
  return { rx: Math.round(150 + 100 * scale), ry: Math.round(140 + 50 * scale) };
}
