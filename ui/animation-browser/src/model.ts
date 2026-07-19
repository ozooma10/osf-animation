export interface SceneRole {
  name: string;
  gender: string;
  filters: Record<string, unknown>;
  equip: boolean;
}

export interface SceneStage {
  index: number;
  name: string;
  tags: string[];
  clipCount: number;
  pinned: number;
  loopSec: number | null;
  timerSec: number | null;
  loops: number | null;
  openEnded: boolean;
  estSec: number | null;
}

export interface SceneModel {
  id: string;
  title: string;
  species: string;
  tags: string[];
  actorCount: number;
  genders: unknown[];
  roles: SceneRole[];
  requiresFurniture: boolean;
  anchors: string[];
  unlisted: boolean;
  wheelCustomized: boolean;
  pinned: number;
  priority: number;
  weight: number;
  sourceFile: string;
  shape: { kind: string; stages: number; nodes: number; branches: number };
  policy: {
    stripActors: "on" | "off" | "inherit";
    lockPlayer: "on" | "off" | "inherit";
    fade: "on" | "off";
    camera: string;
    playerControl: unknown;
  };
  stages: SceneStage[];
  estSec: number | null;
  estPartial: boolean;
  openEnded: boolean;
}

type Raw = Record<string, any>;

export function normalizeScene(raw: Raw): SceneModel {
  const id = String(raw.id || "");
  const actorCount = clampCount(raw.actorCount, raw.roles);
  const genders = Array.isArray(raw.genders) ? raw.genders : [];
  const roles = normalizeRoles(raw.roles, genders, actorCount);
  const tags = Array.isArray(raw.tags) ? raw.tags.map(String) : [];
  const requiresFurniture = Boolean(raw.requiresFurniture || raw.anchorRequired || raw.anchor);
  return {
    id,
    title: String(raw.title || raw.name || id || "Unnamed scene"),
    species: String(raw.species || "human").toLowerCase(),
    tags,
    actorCount,
    genders,
    roles,
    requiresFurniture,
    anchors: Array.isArray(raw.anchors) ? raw.anchors.map(String) : [],
    unlisted: Boolean(raw.unlisted),
    wheelCustomized: Boolean(raw.wheelCustomized),
    pinned: Math.max(0, Math.trunc(Number(raw.pinned) || 0)),
    priority: Number.isFinite(Number(raw.priority)) ? Number(raw.priority) : 0,
    weight: Number.isFinite(Number(raw.weight)) ? Number(raw.weight) : 1,
    sourceFile: String(raw.sourceFile || raw.source || ""),
    shape: normalizeShape(raw, actorCount),
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

export function normalizeStages(stages: unknown): SceneStage[] {
  if (!Array.isArray(stages)) return [];
  return stages.map((raw, index) => {
    const stage: Raw = raw && typeof raw === "object" ? raw : {};
    return {
      index: Number.isInteger(stage.index) ? stage.index : index,
      name: String(stage.name || ""),
      tags: Array.isArray(stage.tags) ? stage.tags.map(String) : [],
      clipCount: Number(stage.clipCount || 0),
      pinned: Math.max(0, Math.trunc(Number(stage.pinned) || 0)),
      loopSec: numberOrNull(stage.loopSec),
      timerSec: numberOrNull(stage.timerSec),
      loops: numberOrNull(stage.loops),
      openEnded: Boolean(stage.openEnded),
      estSec: numberOrNull(stage.estSec),
    };
  });
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

function normalizeRoles(roles: unknown, genders: unknown[], actorCount: number): SceneRole[] {
  if (Array.isArray(roles) && roles.length) {
    return roles.map((raw, index) => {
      const role: Raw = raw && typeof raw === "object" ? raw : {};
      return {
        name: String(role.name || `role ${index + 1}`),
        gender: String(role.gender || role.filters?.gender || "any"),
        filters: role.filters && typeof role.filters === "object" ? role.filters : {},
        equip: Boolean(role.equip),
      };
    });
  }
  const total = actorCount || genders.length;
  return Array.from({ length: total }, (_, index) => ({
    name: `role ${index + 1}`,
    gender: String(genders[index] || "any"),
    filters: {},
    equip: false,
  }));
}

function normalizeShape(raw: Raw, actorCount: number): SceneModel["shape"] {
  if (raw.shape && typeof raw.shape === "object") {
    return {
      kind: String(raw.shape.kind || "linear"),
      stages: Number(raw.shape.stages || raw.shape.stageCount || 0),
      nodes: Number(raw.shape.nodes || raw.shape.nodeCount || 0),
      branches: Number(raw.shape.branches || raw.shape.branchCount || 0),
    };
  }
  const stages = Number(raw.stageCount || raw.linearStageCount || 0);
  const nodes = Number(raw.nodeCount || 0);
  const branches = Number(raw.branchCount || 0);
  return {
    kind: branches > 0 || nodes > stages ? "graph" : "linear",
    stages: stages || (actorCount ? 1 : 0),
    nodes: nodes || stages || 1,
    branches,
  };
}

function policyText(value: unknown, fallback: unknown, empty: "inherit" | "off"): "on" | "off" | "inherit" {
  const resolved = value !== undefined ? value : fallback;
  if (resolved === true) return "on";
  if (resolved === false) return "off";
  return empty;
}

function normalizePolicy(raw: Raw): SceneModel["policy"] {
  const policy: Raw = raw.policy && typeof raw.policy === "object" ? raw.policy : {};
  return {
    stripActors: policyText(policy.stripActors, raw.stripActors, "inherit"),
    lockPlayer: policyText(policy.lockPlayer, raw.lockPlayer, "inherit"),
    fade: policyText(policy.fade, raw.fade, "off") as "on" | "off",
    camera: String(policy.camera || raw.camera || "inherit"),
    playerControl: policy.playerControl || raw.playerControl || null,
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
    blockers.push(`remove ${count} crew member${count === 1 ? "" : "s"}`);
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
    ? "Ready with the current crew and furniture."
    : [...issues, ...blockers].map(sentenceCase).join(". ") + ".";
  return { castCount, actorCount, hasRoles, rolesGate, overCast, anchorGate, seated, issues, blockers, gaps, reason };
}
