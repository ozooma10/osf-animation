import { evaluateScene, type SceneEvaluation, type SceneModel, type SceneStage } from "../model";
import { PLAYER_TOKEN, type ActiveLaunch, type BrowserState, type CastMember, type FurnitureTarget, type PickTarget } from "./state";

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

export type PlayableKind = "animation" | "emote" | "scene";

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

export function sceneCatalog(state: BrowserState): SceneModel[] {
  return state.catalog.filter((scene) => !isEmote(scene) && unlistedVisible(state, scene));
}

export function emoteCatalog(state: BrowserState): SceneModel[] {
  return state.catalog.filter((scene) => isEmote(scene) && unlistedVisible(state, scene));
}

export function playableKey(sceneId: string, stage: number | null): string {
  return `${sceneId}\0${stage == null ? "" : stage}`;
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
    const kind: PlayableKind = isEmote(scene) ? "emote" : "scene";
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
  return `${item.title} ${item.collection} ${scene.title} ${scene.id} ${scene.tags.join(" ")} ${item.stage?.tags.join(" ") ?? ""} ${roles} ${scene.pack} ${scene.sourceFile}`
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
  return item.kind !== "animation" && !item.scene.library && item.scene.unlisted;
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
    ...filteredLibrary(state),
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

export function activeLaunches(state: BrowserState): ActiveLaunch[] {
  if (state.active) return state.active;
  return state.lastHandle
    ? [{ handle: state.lastHandle, sceneId: state.lastSceneId, stage: 0, player: true, cast: [], time: 0, duration: 0, speed: 0 }]
    : [];
}

/** Compatibility spelling aligned with the frozen native `activeScenes` event name. */
export const activeScenes = activeLaunches;

export function busyTokens(state: BrowserState): ReadonlySet<number> {
  const tokens = new Set<number>();
  for (const launch of activeLaunches(state)) {
    for (const member of launch.cast) tokens.add(member.token);
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
  if (!state.preferences.actorLabels || !state.viewVisible || state.minimized) return [];
  return state.cast
    .map((member, index) => ({ member, index }))
    .filter(({ member }) => member.kind !== "player");
}

/** Selected furniture whose world label is live under the same display policy
 *  as selected-cast labels. */
export function labeledFurniture(state: BrowserState): FurnitureTarget | null {
  if (!state.preferences.actorLabels || !state.viewVisible || state.minimized) return null;
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
  return `${scene.title} ${scene.id} ${scene.tags.join(" ")} ${roles} ${scene.pack} ${scene.folder} ${scene.sourceFile}${stages}`
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
    return `-${count} cast`;
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
