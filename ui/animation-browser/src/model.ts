export interface SceneRole {
  name: string;
  gender: string;
}

export interface SceneStage {
  index: number;
  name: string;
  tags: string[];
  clipCount: number;
  loopSec: number | null;
  timerSec: number | null;
  loops: number | null;
  openEnded: boolean;
  estSec: number | null;
}

export type CatalogSourceKind = "authoredScene" | "curatedAnimation" | "referenceAnimation";
export type WorldPlacement = "anchorAndPin" | "followActor";

export interface SceneModel {
  id: string;
  title: string;
  species: string;
  tags: string[];
  actorCount: number;
  roles: SceneRole[];
  requiresFurniture: boolean;
  worldPlacement: WorldPlacement;
  anchors: string[];
  unlisted: boolean;
  priority: number;
  weight: number;
  pack: string;
  folder: string;
  sourceFile: string;
  sourceKind: CatalogSourceKind;
  policy: {
    hideApparel: "on" | "off" | "inherit";
    playerInputLock: "on" | "off" | "inherit";
    fade: "on" | "off";
  };
  stages: SceneStage[];
  estSec: number | null;
  estPartial: boolean;
  openEnded: boolean;
  /** View-only marker for generated reference-library entries. */
  library?: boolean;
  /** Lower-cased stage-name search index, populated when library data arrives. */
  stageHay?: string;
}

type Raw = Record<string, any>;

export function normalizeScene(raw: Raw): SceneModel {
  const id = String(raw.id || "");
  const actorCount = clampCount(raw.actorCount, raw.roles);
  const roles = normalizeRoles(raw.roles, actorCount);
  const tags = Array.isArray(raw.tags) ? raw.tags.map(String) : [];
  const requiresFurniture = Boolean(raw.requiresFurniture);
  const sourceKind: CatalogSourceKind = ["authoredScene", "curatedAnimation", "referenceAnimation"].includes(String(raw.sourceKind))
    ? raw.sourceKind as CatalogSourceKind
    : id.toLowerCase().startsWith("osf.scene-clip/")
      ? "curatedAnimation"
      : "authoredScene";
  const worldPlacement: WorldPlacement = raw.placement === "anchorAndPin" || raw.placement === "followActor"
    ? raw.placement
    : Boolean(raw.inPlace) ? "followActor" : "anchorAndPin";
  return {
    id,
    title: String(raw.title || raw.name || id || "Unnamed scene"),
    species: String(raw.species || "human").toLowerCase(),
    tags,
    actorCount,
    roles,
    requiresFurniture,
    worldPlacement,
    anchors: Array.isArray(raw.anchors) ? raw.anchors.map(String) : [],
    unlisted: Boolean(raw.unlisted),
    priority: Number.isFinite(Number(raw.priority)) ? Number(raw.priority) : 0,
    weight: Number.isFinite(Number(raw.weight)) ? Number(raw.weight) : 1,
    pack: String(raw.pack || "").trim(),
    folder: normalizeFolder(raw.folder),
    sourceFile: String(raw.sourceFile || raw.source || ""),
    sourceKind,
    policy: normalizePolicy(raw),
    stages: normalizeStages(raw.stages),
    estSec: numberOrNull(raw.estSec),
    estPartial: Boolean(raw.estPartial),
    openEnded: Boolean(raw.openEnded),
  };
}

export function safeNormalizeScene(raw: unknown): SceneModel | null {
  try {
    return raw && typeof raw === "object" ? normalizeScene(raw as Raw) : null;
  } catch {
    return null;
  }
}

/** A whole catalog payload — the scenes lane, or the library lane with `library` set. */
export function normalizeCatalog(payload: unknown, library = false): SceneModel[] {
  if (!Array.isArray(payload)) return [];
  return payload.map(safeNormalizeScene).filter((scene): scene is SceneModel => !!scene).map((scene) => library
    ? { ...scene, library: true, sourceKind: scene.sourceKind === "authoredScene" ? "referenceAnimation" : scene.sourceKind, stageHay: scene.stages.map((stage) => stage.name).join(" ").toLowerCase() }
    : scene);
}

export function normalizeStages(stages: unknown): SceneStage[] {
  if (!Array.isArray(stages)) return [];
  return stages.map((raw, index) => {
    const stage: Raw = raw && typeof raw === "object" ? raw : {};
    return {
      index: Number.isInteger(stage.index) ? stage.index : index,
      name: String(stage.name || ""),
      tags: Array.isArray(stage.tags) ? stage.tags.map(String) : [],
      clipCount: Number(stage.clipCount || 0),
      loopSec: numberOrNull(stage.loopSec),
      timerSec: numberOrNull(stage.timerSec),
      loops: numberOrNull(stage.loops),
      openEnded: Boolean(stage.openEnded),
      estSec: numberOrNull(stage.estSec),
    };
  });
}

function normalizeFolder(value: unknown): string {
  if (typeof value !== "string") return "";
  const segments = value.replace(/\\/g, "/").split("/").map((segment) => segment.trim());
  if (segments.some((segment) => !segment || segment === "." || segment === "..")) return "";
  return segments.join("/");
}

function numberOrNull(value: unknown): number | null {
  const number = Number(value);
  return value == null || !Number.isFinite(number) ? null : number;
}

function clampCount(actorCount: unknown, roles: unknown): number {
  if (Number.isFinite(Number(actorCount)) && Number(actorCount) > 0) return Number(actorCount);
  if (Array.isArray(roles) && roles.length) return roles.length;
  return 0;
}

function normalizeRoles(roles: unknown, actorCount: number): SceneRole[] {
  if (Array.isArray(roles) && roles.length) {
    return roles.map((raw, index) => {
      const role: Raw = raw && typeof raw === "object" ? raw : {};
      return {
        name: String(role.name || `role ${index + 1}`),
        gender: String(role.gender || role.filters?.gender || "any"),
      };
    });
  }
  return Array.from({ length: actorCount }, (_, index) => ({
    name: `role ${index + 1}`,
    gender: "any",
  }));
}

function policyText(value: unknown, fallback: unknown, empty: "inherit" | "off"): "on" | "off" | "inherit" {
  const resolved = value !== undefined ? value : fallback;
  if (resolved === true) return "on";
  if (resolved === false) return "off";
  return empty;
}

function normalizePolicy(raw: Raw): SceneModel["policy"] {
  return {
    hideApparel: policyText(raw.hideApparel, raw.stripActors, "inherit"),
    playerInputLock: policyText(raw.playerInputLock, raw.lockPlayer, "inherit"),
    fade: policyText(raw.fade, undefined, "off") as "on" | "off",
  };
}

export interface SceneEvaluationContext {
  castCount: number;
  furnitureToken: number | null;
  anchorMatch: { token: number; ids: ReadonlySet<string> } | null;
}

export interface SceneEvaluation {
  castCount: number;
  actorCount: number;
  hasRoles: boolean;
  rolesGate: boolean;
  overCast: boolean;
  anchorGate: boolean;
  seated: number;
  issues: string[];
  blockers: string[];
  gaps: number;
  reason: string;
}

export function evaluateScene(scene: SceneModel, context: SceneEvaluationContext): SceneEvaluation {
  const { castCount } = context;
  const actorCount = scene.actorCount || 0;
  const hasRoles = actorCount > 0;
  const rolesGate = hasRoles && castCount >= actorCount;
  const overCast = hasRoles && castCount > actorCount;
  const matchKnown = context.furnitureToken != null
    && context.anchorMatch?.token === context.furnitureToken;
  const anchorFits = matchKnown ? context.anchorMatch!.ids.has(scene.id) : true;
  const anchorGate = scene.requiresFurniture ? context.furnitureToken != null && anchorFits : true;
  const seated = hasRoles ? Math.min(castCount, actorCount) : 0;
  const issues: string[] = [];
  const blockers: string[] = [];

  if (!hasRoles) blockers.push("scene defines no roles");
  else if (!rolesGate) {
    const count = actorCount - castCount;
    issues.push(`needs ${count} more actor${count === 1 ? "" : "s"}`);
  }
  if (overCast) {
    const count = castCount - actorCount;
    blockers.push(`remove ${count} cast member${count === 1 ? "" : "s"}`);
  }
  if (!anchorGate) {
    const anchors = scene.anchors.join(" / ");
    issues.push(context.furnitureToken != null
      ? `this furniture doesn't fit${anchors ? ` (needs ${anchors})` : ""}`
      : anchors ? `needs ${anchors}` : "needs furniture");
  }
  const gaps = issues.length + blockers.length;
  const sentenceCase = (text: string) => text ? text.charAt(0).toUpperCase() + text.slice(1) : text;
  const reason = gaps === 0
    ? "Ready with the current cast and furniture."
    : [...issues, ...blockers].map(sentenceCase).join(". ") + ".";
  return { castCount, actorCount, hasRoles, rolesGate, overCast, anchorGate, seated, issues, blockers, gaps, reason };
}
