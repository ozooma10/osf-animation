export interface SceneRole {
  name: string;
  gender: string;
}

export type TimelineTrackKind = "cue" | "action" | "sound" | "camera";
/** "unreachable" = an `atFrame` at/past the clip end — the runtime never fires it. */
export type TimelineTrackPosition = "enter" | "exit" | "end" | "fraction" | "unreachable";

export interface TimelineTrackMark {
  kind: TimelineTrackKind;
  /** Clip-local position: enter maps to 0, exit/end to 1. */
  at: number;
  /** Authored `atFrame` position in seconds; lets the rail re-place the mark against the
   *  live decoded duration (the pack-authored clip length can disagree with it). */
  atSec?: number;
  trackPosition: TimelineTrackPosition;
  label: string;
  detail: string;
  role: string;
  repeat: boolean;
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
  tracks: TimelineTrackMark[];
}

export type CatalogSourceKind = "authoredScene" | "curatedAnimation" | "derivedDebugAnimation" | "referenceAnimation";
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
  /** Legacy bridge flag retained while older native builds do not emit `sourceKind`. */
  curated: boolean;
  wheelCustomized: boolean;
  pinned: number;
  priority: number;
  weight: number;
  pack: string;
  folder: string;
  sourceFile: string;
  sourcePath: string;
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
  const sourceKind: CatalogSourceKind = ["authoredScene", "curatedAnimation", "derivedDebugAnimation", "referenceAnimation"].includes(String(raw.sourceKind))
    ? raw.sourceKind as CatalogSourceKind
    : Boolean(raw.curated)
      ? "curatedAnimation"
      : id.toLowerCase().startsWith("osf.scene-clip/")
        ? "derivedDebugAnimation"
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
    curated: sourceKind === "curatedAnimation",
    wheelCustomized: Boolean(raw.wheelCustomized),
    pinned: Math.max(0, Math.trunc(Number(raw.pinned) || 0)),
    priority: Number.isFinite(Number(raw.priority)) ? Number(raw.priority) : 0,
    weight: Number.isFinite(Number(raw.weight)) ? Number(raw.weight) : 1,
    pack: String(raw.pack || "").trim(),
    folder: normalizeFolder(raw.folder),
    sourceFile: String(raw.sourceFile || raw.source || ""),
    sourcePath: String(raw.sourcePath || raw.sourceFile || raw.source || "").replace(/\\/g, "/"),
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
      pinned: Math.max(0, Math.trunc(Number(stage.pinned) || 0)),
      loopSec: numberOrNull(stage.loopSec),
      timerSec: numberOrNull(stage.timerSec),
      loops: numberOrNull(stage.loops),
      openEnded: Boolean(stage.openEnded),
      estSec: numberOrNull(stage.estSec),
      tracks: normalizeTimelineTracks(stage.tracks),
    };
  });
}

const TRACK_KINDS = new Set<TimelineTrackKind>(["cue", "action", "sound", "camera"]);
const TRACK_POSITIONS = new Set<TimelineTrackPosition>(["enter", "exit", "end", "fraction", "unreachable"]);

export function normalizeTimelineTracks(value: unknown): TimelineTrackMark[] {
  if (!Array.isArray(value)) return [];
  return value.flatMap((raw): TimelineTrackMark[] => {
    if (!raw || typeof raw !== "object") return [];
    const mark = raw as Raw;
    const kind = String(mark.kind || "") as TimelineTrackKind;
    // `anchor` is the native bridge's legacy field. Normalized UI state uses the
    // temporal term `trackPosition` so it cannot be confused with a world anchor.
    const trackPosition = String(mark.trackPosition ?? mark.anchor ?? "fraction") as TimelineTrackPosition;
    const at = Number(mark.at);
    const label = String(mark.label || "").trim();
    if (!TRACK_KINDS.has(kind) || !TRACK_POSITIONS.has(trackPosition) || !Number.isFinite(at) || !label) return [];
    const atSec = Number(mark.atSec);
    return [{
      kind,
      at: Math.max(0, Math.min(1, at)),
      ...(Number.isFinite(atSec) && atSec >= 0 ? { atSec } : {}),
      trackPosition,
      label,
      detail: String(mark.detail || "").trim(),
      role: String(mark.role || "").trim(),
      repeat: Boolean(mark.repeat),
    }];
  });
}

export interface RouteLayerModel {
  clip: string;
  animId: string;
  durationHint: number | null;
  mask: string;
  mode: string;
  weight: number;
  holdAt: number | null;
}

export interface RouteStationModel {
  id: string;
  layer: RouteLayerModel | null;
}

export type RouteEventKind = "commit" | "marker" | "prop" | "sound";

export interface RouteEventModel {
  kind: RouteEventKind;
  frame: number;
  label: string;
  detail: string;
  lifetime: string;
  external: boolean;
}

export interface RouteTransitionModel {
  id: string;
  from: string;
  to: string;
  layer: RouteLayerModel;
  interruption: "finish-transition" | "crossfade-before-commit";
  events: RouteEventModel[];
}

export interface RouteModel {
  id: string;
  sourceFile: string;
  stations: RouteStationModel[];
  transitions: RouteTransitionModel[];
}

function normalizeRouteLayer(value: unknown): RouteLayerModel {
  const raw: Raw = value && typeof value === "object" ? value as Raw : {};
  const durationHint = Number(raw.durationHint);
  const holdAt = Number(raw.holdAt);
  return {
    clip: String(raw.clip || ""),
    animId: String(raw.animId || ""),
    durationHint: Number.isFinite(durationHint) && durationHint > 0 ? durationHint : null,
    mask: String(raw.mask || ""),
    mode: String(raw.mode || "override"),
    weight: Number.isFinite(Number(raw.weight)) ? Number(raw.weight) : 1,
    holdAt: Number.isFinite(holdAt) && holdAt >= 0 ? holdAt : null,
  };
}

function routeFrame(value: unknown): number {
  const frame = Number(value);
  return Number.isFinite(frame) ? Math.max(0, frame) : 0;
}

function normalizeRouteEvents(raw: Raw): RouteEventModel[] {
  const events: RouteEventModel[] = [];
  if (raw.commit && typeof raw.commit === "object") {
    const commit = raw.commit as Raw;
    events.push({ kind: "commit", frame: routeFrame(commit.frame), label: String(commit.id || "commit"),
      detail: "Consumer handoff checkpoint · callback suppressed in preview", lifetime: "", external: true });
  }
  if (Array.isArray(raw.markers)) raw.markers.forEach((value) => {
    if (!value || typeof value !== "object") return;
    const marker = value as Raw;
    events.push({ kind: "marker", frame: routeFrame(marker.frame), label: String(marker.id || "marker"),
      detail: "Consumer marker callback suppressed in preview", lifetime: "", external: true });
  });
  if (Array.isArray(raw.props)) raw.props.forEach((value) => {
    if (!value || typeof value !== "object") return;
    const prop = value as Raw;
    const lifetime = String(prop.lifetime || "transition");
    const attach = Boolean(prop.attach);
    const external = lifetime === "external";
    const attachmentNode = prop.attachmentNode ?? prop.node;
    events.push({ kind: "prop", frame: routeFrame(prop.frame),
      label: `${attach ? "ATTACH" : "DESTROY"} ${String(prop.id || "prop")}`,
      detail: [lifetime && `${lifetime} lifetime`, attachmentNode && `attachment node ${String(attachmentNode)}`,
        external && "consumer callback suppressed"].filter(Boolean).join(" · "),
      lifetime, external });
  });
  if (Array.isArray(raw.sounds)) raw.sounds.forEach((value) => {
    if (!value || typeof value !== "object") return;
    const sound = value as Raw;
    events.push({ kind: "sound", frame: routeFrame(sound.frame), label: String(sound.spec || "sound"),
      detail: "Audio suppressed in preview", lifetime: "", external: true });
  });
  return events.sort((a, b) => a.frame - b.frame);
}

export function normalizeRouteCatalog(payload: unknown): RouteModel[] {
  if (!Array.isArray(payload)) return [];
  return payload.flatMap((value): RouteModel[] => {
    if (!value || typeof value !== "object") return [];
    const raw = value as Raw;
    const id = String(raw.id || "").trim();
    if (!id) return [];
    const stations = Array.isArray(raw.stations) ? raw.stations.flatMap((value): RouteStationModel[] => {
      if (!value || typeof value !== "object") return [];
      const station = value as Raw;
      const stationId = String(station.id || "").trim();
      return stationId ? [{ id: stationId, layer: station.layer && typeof station.layer === "object"
        ? normalizeRouteLayer(station.layer) : null }] : [];
    }) : [];
    const transitions = Array.isArray(raw.transitions) ? raw.transitions.flatMap((value): RouteTransitionModel[] => {
      if (!value || typeof value !== "object") return [];
      const transition = value as Raw;
      const transitionId = String(transition.id || "").trim();
      const from = String(transition.from || "").trim();
      const to = String(transition.to || "").trim();
      if (!transitionId || !from || !to) return [];
      return [{
        id: transitionId,
        from,
        to,
        layer: normalizeRouteLayer(transition.layer),
        // The route schema's legacy `finish` value means finish the current
        // transition, not finish the whole route. Normalize that scope here.
        interruption: transition.interruption === "crossfade-before-commit"
          ? "crossfade-before-commit" : "finish-transition",
        events: normalizeRouteEvents(transition),
      }];
    }) : [];
    return [{ id, sourceFile: String(raw.sourceFile || "").replace(/\\/g, "/"), stations, transitions }];
  }).sort((a, b) => a.id.localeCompare(b.id));
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

// ---- import report (osf.animation.imports.data) -----------------------------------------------
// A FILE-shaped view of the last registry load, deliberately unlike the scene-shaped catalog: a
// file that produced no scenes has nothing to appear as in the catalog, and "why is my pack
// missing" is exactly the question only this shape can answer.

/** What one *.osf.json contributed to the registry. */
export interface ImportProblem {
  severity: "error" | "warn";
  code: string;
  message: string;
  hint: string;
  scene: string;
  node: string;
  role: string;
  clip: string;
}

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
  declaredScenes: number;
  hidden: number;
  rejectedScenes: number;
  routes: number;
  declaredRoutes: number;
  rejectedRoutes: number;
  unlisted: number;
  anchored: number;
  nodes: number;
  stages: number;
  roles: number;
  clips: number;
  distinctClips: number;
  missingClips: number;
  missingClipExamples: string[];
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
  problems: ImportProblem[];
  problemCount: number;
}

export interface ImportTotals {
  files: number;
  rejectedFiles: number;
  scenes: number;
  declaredScenes: number;
  rejectedScenes: number;
  routes: number;
  declaredRoutes: number;
  rejectedRoutes: number;
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
  declaredScenes: 0,
  rejectedScenes: 0,
  routes: 0,
  declaredRoutes: 0,
  rejectedRoutes: 0,
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

function normalizeImportProblem(value: unknown): ImportProblem {
  if (typeof value === "string") {
    const severity = value.startsWith("[warn]") ? "warn" : "error";
    return {
      severity,
      code: severity === "warn" ? "legacy-warning" : "legacy-error",
      message: value.replace(/^\[(?:warn|error)\]\s*/, ""),
      hint: "",
      scene: "",
      node: "",
      role: "",
      clip: "",
    };
  }
  const problem: Raw = value && typeof value === "object" ? value as Raw : {};
  return {
    severity: problem.severity === "warn" ? "warn" : "error",
    code: String(problem.code || "load-problem"),
    message: String(problem.message || "Unknown import problem.").replace(/^\[(?:warn|error)\]\s*/, ""),
    hint: String(problem.hint || ""), scene: String(problem.scene || ""),
    node: String(problem.node || ""), role: String(problem.role || ""), clip: String(problem.clip || ""),
  };
}

export function normalizeImportReport(payload: unknown): ImportReport {
  const raw: Raw = payload && typeof payload === "object" ? (payload as Raw) : {};
  const files = Array.isArray(raw.files) ? raw.files : [];
  const totals: Raw = raw.totals && typeof raw.totals === "object" ? raw.totals : {};
  return {
    files: files.map((value): ImportFile => {
      const entry: Raw = value && typeof value === "object" ? value : {};
      const problems = Array.isArray(entry.problems) ? entry.problems.map(normalizeImportProblem) : [];
      return {
        path: String(entry.path || ""),
        file: String(entry.file || ""),
        pack: String(entry.pack || "").trim(),
        library: Boolean(entry.library),
        schema: count(entry.schema),
        bytes: count(entry.bytes),
        parseMs: Number.isFinite(Number(entry.parseMs)) ? Math.max(0, Number(entry.parseMs)) : 0,
        scenes: count(entry.scenes),
        declaredScenes: count(entry.declaredScenes),
        hidden: count(entry.hidden),
        rejectedScenes: count(entry.rejectedScenes),
        routes: count(entry.routes),
        declaredRoutes: count(entry.declaredRoutes),
        rejectedRoutes: count(entry.rejectedRoutes),
        unlisted: count(entry.unlisted),
        anchored: count(entry.anchored),
        nodes: count(entry.nodes),
        stages: count(entry.stages),
        roles: count(entry.roles),
        clips: count(entry.clips),
        distinctClips: count(entry.distinctClips),
        missingClips: count(entry.missingClips),
        missingClipExamples: Array.isArray(entry.missingClipExamples) ? entry.missingClipExamples.map(String) : [],
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
      declaredScenes: count(totals.declaredScenes),
      rejectedScenes: count(totals.rejectedScenes),
      scenes: count(totals.scenes),
      routes: count(totals.routes),
      declaredRoutes: count(totals.declaredRoutes),
      rejectedRoutes: count(totals.rejectedRoutes),
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

export interface ImportProblemRef {
  key: string;
  path: string;
  file: string;
  code: string;
  message: string;
}

export interface ImportReloadDelta {
  newProblems: ImportProblemRef[];
  resolvedProblems: ImportProblemRef[];
  changedFiles: number;
  addedFiles: number;
  removedFiles: number;
}

export function importProblemKey(path: string, problem: ImportProblem): string {
  return `${path || "\0cross-file"}\n${problem.code}\n${problem.message}`;
}

function problemRefs(files: readonly ImportFile[]): Map<string, ImportProblemRef> {
  const refs = new Map<string, ImportProblemRef>();
  for (const file of files) {
    for (const problem of file.problems) {
      const key = importProblemKey(file.path, problem);
      refs.set(key, { key, path: file.path, file: file.file, code: problem.code, message: problem.message });
    }
  }
  return refs;
}

function importFileFingerprint(file: ImportFile): string {
  return [
    file.declaredScenes, file.scenes, file.rejectedScenes, file.hidden, file.missingClips,
    file.declaredRoutes, file.routes, file.rejectedRoutes,
    file.clipEntries, file.errors, file.warnings, file.problemCount,
  ].join(":");
}

export function diffImportReports(before: readonly ImportFile[], after: readonly ImportFile[]): ImportReloadDelta {
  const previousProblems = problemRefs(before);
  const currentProblems = problemRefs(after);
  const newProblems = [...currentProblems].filter(([key]) => !previousProblems.has(key)).map(([, value]) => value);
  const resolvedProblems = [...previousProblems].filter(([key]) => !currentProblems.has(key)).map(([, value]) => value);

  const previousFiles = new Map(before.map((file) => [file.path || "\0cross-file", file]));
  const currentFiles = new Map(after.map((file) => [file.path || "\0cross-file", file]));
  let changedFiles = 0;
  for (const [path, file] of currentFiles) {
    const previous = previousFiles.get(path);
    if (previous && importFileFingerprint(previous) !== importFileFingerprint(file)) changedFiles++;
  }
  return {
    newProblems,
    resolvedProblems,
    changedFiles,
    addedFiles: [...currentFiles.keys()].filter((key) => !previousFiles.has(key)).length,
    removedFiles: [...previousFiles.keys()].filter((key) => !currentFiles.has(key)).length,
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
