import { evaluateScene, type SceneEvaluation, type SceneModel, type SceneStage } from "../model";
import type { ActiveScene, BrowserState } from "./state";

export const WHEEL_MAX = 12;

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
  return state.catalog.filter(isEmote);
}

export function animationList(state: BrowserState): SceneModel[] {
  return [...emoteCatalog(state), ...filteredLibrary(state)];
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
  return sceneById(state, id)?.title ?? (id || "scene");
}

export function activeScenes(state: BrowserState): ActiveScene[] {
  if (state.active) return state.active;
  return state.lastHandle
    ? [{ handle: state.lastHandle, sceneId: state.lastSceneId, stage: 0, player: true, cast: [] }]
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

export function partnerCount(state: BrowserState): number {
  return state.cast.reduce((count, member) => count + (member.kind === "player" ? 0 : 1), 0);
}

export function unlistedVisible(state: BrowserState, scene: SceneModel): boolean {
  return !!scene.library || !scene.unlisted || state.filters.debugMode;
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
  return `${scene.title} ${scene.id} ${scene.tags.join(" ")} ${roles} ${scene.pack} ${scene.sourceFile}${stages}`
    .toLowerCase()
    .includes(state.filters.search);
}

export function browseVisible(state: BrowserState, scene: SceneModel): boolean {
  if (state.mode === "library" && state.libCustomOnly && isVanillaAnimation(scene)) return false;
  if (!matchesSearch(state, scene) || !speciesVisible(state, scene)) return false;
  return state.mode !== "scenes" || state.browseAll || evaluateForState(state, scene).gaps === 0;
}

export function selectionCandidates(state: BrowserState): SceneModel[] {
  if (state.mode === "active") return state.catalog;
  return state.mode === "library" ? animationList(state) : sceneCatalog(state);
}

export function validSelection(state: BrowserState): string | null {
  if (state.mode === "active" && state.selectedId && sceneById(state, state.selectedId)) return state.selectedId;
  const candidates = selectionCandidates(state);
  const visible = candidates.filter((scene) => browseVisible(state, scene));
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

export function fitsKeyedAnchor(state: BrowserState, scene: SceneModel): boolean | null {
  if (!scene.requiresFurniture) return null;
  if (!state.furniture || state.anchorMatch?.token !== state.furniture.token) return null;
  return state.anchorMatch.ids.has(scene.id);
}

export function libraryRank(state: BrowserState, scene: SceneModel): number {
  if (!scene.requiresFurniture) return state.furniture ? 1 : 0;
  return fitsKeyedAnchor(state, scene) === true ? 0 : 2;
}

export function stageClean(stage: SceneStage): boolean {
  return !stage.tags.includes("transition") && !stage.tags.includes("partial");
}

export function cleanStages(scene: SceneModel): SceneStage[] {
  return scene.stages.filter(stageClean);
}

export function setQuality(scene: SceneModel): number {
  const photomode = /(^|\/)photomode(\/|$)/.test(scene.id);
  const clean = cleanStages(scene);
  const poseMajority = clean.length > 0 && clean.filter((stage) => stage.tags.includes("pose")).length > clean.length / 2;
  if (photomode && poseMajority) return 0;
  if (photomode) return 1;
  if (poseMajority) return 2;
  if (clean.some((stage) => /idle/i.test(stage.name) || stage.tags.some((tag) => tag.startsWith("idle")))) return 3;
  return 4;
}

export function wheelKey(scene: string, stage: number | null): string {
  return `${scene}\0${stage == null ? "" : stage}`;
}

export function wheelStageTitle(scene: SceneModel, stage: SceneStage): string {
  if (stage.name) return stage.name;
  return scene.stages.length === 1 ? scene.title : `${scene.title} · Stage ${stage.index + 1}`;
}

export function wheelCandidates(state: BrowserState): WheelCandidate[] {
  const candidates: WheelCandidate[] = emoteCatalog(state).filter(isWheelEmote).map((scene) => ({
    key: wheelKey(scene.id, null),
    scene: scene.id,
    stage: null,
    title: scene.title,
    detail: scene.title,
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
        detail: stage.name ? `${scene.title} · ${stage.name}` : title,
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
  return scene.stages.find((stage) => stage.index === index)?.name || `stage ${index}`;
}

export function wheelGeometry(count: number): { rx: number; ry: number } {
  const scale = Math.max(0, Math.min(1, (count - 3) / 9));
  return { rx: Math.round(150 + 100 * scale), ry: Math.round(140 + 50 * scale) };
}
