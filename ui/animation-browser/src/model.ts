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
  inPlace: boolean;
  anchors: string[];
  unlisted: boolean;
  /** A generated one-clip entry a pack registered via `clipLibrary` — authored content that
   *  happens to share the `osf.scene-clip/` id namespace with the auto-harvested debug entries. */
  curated: boolean;
  wheelCustomized: boolean;
  pinned: number;
  priority: number;
  weight: number;
  pack: string;
  folder: string;
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
  /** View-only marker for generated reference-library entries. */
  library?: boolean;
  /** Lower-cased stage-name search index, populated when library data arrives. */
  stageHay?: string;
}

type Raw = Record<string, any>;

export function normalizeScene(raw: Raw): SceneModel {
  const id = String(raw.id || "");
  const actorCount = clampCount(raw.actorCount, raw.roles);
  const genders = Array.isArray(raw.genders) ? raw.genders : [];
  const roles = normalizeRoles(raw.roles, genders, actorCount);
  const tags = Array.isArray(raw.tags) ? raw.tags.map(String) : [];
  const requiresFurniture = Boolean(raw.requiresFurniture);
  return {
    id,
    title: String(raw.title || raw.name || id || "Unnamed scene"),
    species: String(raw.species || "human").toLowerCase(),
    tags,
    actorCount,
    genders,
    roles,
    requiresFurniture,
    inPlace: Boolean(raw.inPlace),
    anchors: Array.isArray(raw.anchors) ? raw.anchors.map(String) : [],
    unlisted: Boolean(raw.unlisted),
    curated: Boolean(raw.curated),
    wheelCustomized: Boolean(raw.wheelCustomized),
    pinned: Math.max(0, Math.trunc(Number(raw.pinned) || 0)),
    priority: Number.isFinite(Number(raw.priority)) ? Number(raw.priority) : 0,
    weight: Number.isFinite(Number(raw.weight)) ? Number(raw.weight) : 1,
    pack: String(raw.pack || "").trim(),
    folder: normalizeFolder(raw.folder),
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

/** A whole catalog payload — the scenes lane, or the library lane with `library` set. */
export function normalizeCatalog(payload: unknown, library = false): SceneModel[] {
  if (!Array.isArray(payload)) return [];
  return payload.map(safeNormalizeScene).filter((scene): scene is SceneModel => !!scene).map((scene) => library
    ? { ...scene, library: true, stageHay: scene.stages.map((stage) => stage.name).join(" ").toLowerCase() }
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
      pinned: Math.max(0, Math.trunc(Number(stage.pinned) || 0)),
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

// Derived from the only shape signal the bridge actually sends (stageCount): the card
// payload has no shape/nodeCount/branchCount keys, so a graph scene simply reports its
// linear stages (0) — the old alias keys were a contract the native side never honoured.
function normalizeShape(raw: Raw, actorCount: number): SceneModel["shape"] {
  const stages = Number(raw.stageCount || 0);
  return {
    kind: "linear",
    stages: stages || (actorCount ? 1 : 0),
    nodes: stages || 1,
    branches: 0,
  };
}

function policyText(value: unknown, fallback: unknown, empty: "inherit" | "off"): "on" | "off" | "inherit" {
  const resolved = value !== undefined ? value : fallback;
  if (resolved === true) return "on";
  if (resolved === false) return "off";
  return empty;
}

function normalizePolicy(raw: Raw): SceneModel["policy"] {
  return {
    stripActors: policyText(raw.stripActors, undefined, "inherit"),
    lockPlayer: policyText(raw.lockPlayer, undefined, "inherit"),
    fade: policyText(raw.fade, undefined, "off") as "on" | "off",
    camera: String(raw.camera || "inherit"),
    playerControl: raw.playerControl || null,
  };
}

// ---- import report (osf.animation.imports.data) -----------------------------------------------
// A FILE-shaped view of the last registry load, deliberately unlike the scene-shaped catalog: a
// file that produced no scenes has nothing to appear as in the catalog, and "why is my pack
// missing" is exactly the question only this shape can answer.

/** What one *.osf.json contributed to the registry. */
export interface ImportFile {
  /** Data/OSF-relative, forward-slashed. Empty on the trailing cross-file problem bucket. */
  path: string;
  file: string;
  pack: string;
  library: boolean;
  /** Declared `schema` — 0 when absent, non-integer, or the file never parsed. */
  schema: number;
  bytes: number;
  parseMs: number;
  scenes: number;
  hidden: number;
  unlisted: number;
  anchored: number;
  nodes: number;
  stages: number;
  roles: number;
  clips: number;
  distinctClips: number;
  missingClips: number;
  cues: number;
  actions: number;
  sounds: number;
  cameras: number;
  clipEntries: number;
  species: string[];
  errors: number;
  warnings: number;
  /** Contributed nothing and reported at least one error. */
  rejected: boolean;
  /** Bounded by the native side; `problemCount` is the true total. */
  problems: string[];
  problemCount: number;
}

export interface ImportTotals {
  files: number;
  rejectedFiles: number;
  scenes: number;
  /** The registry's own authored count, so a drift from the per-file sum stays visible. */
  registered: number;
  clipEntries: number;
  hidden: number;
  missingClips: number;
  errors: number;
  warnings: number;
  bytes: number;
  parseMs: number;
}

export interface ImportReport {
  files: ImportFile[];
  totals: ImportTotals;
}

export const EMPTY_IMPORT_TOTALS: ImportTotals = {
  files: 0,
  rejectedFiles: 0,
  scenes: 0,
  registered: 0,
  clipEntries: 0,
  hidden: 0,
  missingClips: 0,
  errors: 0,
  warnings: 0,
  bytes: 0,
  parseMs: 0,
};

function count(value: unknown): number {
  const number = Math.trunc(Number(value));
  return Number.isFinite(number) && number > 0 ? number : 0;
}

export function normalizeImportReport(payload: unknown): ImportReport {
  const raw: Raw = payload && typeof payload === "object" ? (payload as Raw) : {};
  const files = Array.isArray(raw.files) ? raw.files : [];
  const totals: Raw = raw.totals && typeof raw.totals === "object" ? raw.totals : {};
  return {
    files: files.map((value): ImportFile => {
      const entry: Raw = value && typeof value === "object" ? value : {};
      const problems = Array.isArray(entry.problems) ? entry.problems.map(String) : [];
      return {
        path: String(entry.path || ""),
        file: String(entry.file || ""),
        pack: String(entry.pack || "").trim(),
        library: Boolean(entry.library),
        schema: count(entry.schema),
        bytes: count(entry.bytes),
        parseMs: Number.isFinite(Number(entry.parseMs)) ? Math.max(0, Number(entry.parseMs)) : 0,
        scenes: count(entry.scenes),
        hidden: count(entry.hidden),
        unlisted: count(entry.unlisted),
        anchored: count(entry.anchored),
        nodes: count(entry.nodes),
        stages: count(entry.stages),
        roles: count(entry.roles),
        clips: count(entry.clips),
        distinctClips: count(entry.distinctClips),
        missingClips: count(entry.missingClips),
        cues: count(entry.cues),
        actions: count(entry.actions),
        sounds: count(entry.sounds),
        cameras: count(entry.cameras),
        clipEntries: count(entry.clipEntries),
        species: Array.isArray(entry.species) ? entry.species.map(String) : [],
        errors: count(entry.errors),
        warnings: count(entry.warnings),
        rejected: Boolean(entry.rejected),
        problems,
        // Never let a stale/short `problemCount` claim fewer lines than actually arrived.
        problemCount: Math.max(count(entry.problemCount), problems.length),
      };
    }),
    totals: {
      files: count(totals.files),
      rejectedFiles: count(totals.rejectedFiles),
      scenes: count(totals.scenes),
      registered: count(totals.registered),
      clipEntries: count(totals.clipEntries),
      hidden: count(totals.hidden),
      missingClips: count(totals.missingClips),
      errors: count(totals.errors),
      warnings: count(totals.warnings),
      bytes: count(totals.bytes),
      parseMs: Number.isFinite(Number(totals.parseMs)) ? Math.max(0, Number(totals.parseMs)) : 0,
    },
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
