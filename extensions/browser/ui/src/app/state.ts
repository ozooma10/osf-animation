import { EMPTY_IMPORT_TOTALS, type ImportFile, type ImportReloadDelta, type ImportTotals, type SceneModel } from "../model";

export const PLAYER_TOKEN = -1;

export interface CastMember {
  token: number;
  name: string;
  kind?: "player";
  distance?: number | null;
  species: string;
  /** "" when the actor has no actorbase sex (most creatures). */
  sex: string;
}

export const PLAYER_CAST: CastMember = {
  token: PLAYER_TOKEN,
  name: "Player",
  kind: "player",
  species: "human",
  // Player sex arrives via PluginVersion.playerSex.
  sex: "",
};

export interface NearbyTarget {
  token: number;
  name: string;
  formId: number;
  distance: number | null;
  isActor: boolean;
  species: string;
  sex: string;
  sceneCount: number | null;
  customCount: number | null;
  marker: boolean;
}

export interface ActiveCastMember {
  token: number;
  name: string;
  player: boolean;
}

export interface ActiveLaunch {
  handle: number;
  sceneId: string;
  stage: number;
  player: boolean;
  cast: ActiveCastMember[];
  time: number;
  duration: number;
  speed: number;
}

export type LocationMode = "cast" | "player" | "actor" | "front" | "furniture";

export interface FurnitureTarget {
  token: number;
  name: string;
  distance: number | null;
}

export interface PluginVersion {
  plugin?: string;
  version?: string;
  relativePointer?: boolean;
  playerSex?: string;
}

export interface LaunchOptions {
  hideApparel: "-1" | "0" | "1";
  playerInputLock: "-1" | "0" | "1";
  camera: string;
  speed: string;
}

export type BrowserMode = "scenes" | "library" | "active" | "wheel";
export type BrowseMode = Exclude<BrowserMode, "wheel">;
export type BrowseKind = "all" | "animation" | "emote" | "scene";
/** Older saved snapshots still say `action`. */
export type BrowseKindInput = BrowseKind | "action";

export function normalizeBrowseKind(value: BrowseKindInput): BrowseKind {
  return value === "action" ? "emote" : value;
}
export type AfterLaunch = "minimize" | "stay" | "close";
export type OpenTo = "last" | BrowseMode;
export type UnavailableScenes = "ask" | "show" | "hide";
export type ImportFilter = "attention" | "rejected" | "partial" | "missing" | "empty" | "all";

export interface ImportReloadState {
  status: "idle" | "running" | "success" | "error";
  completedAt: number;
  durationMs: number;
  scenes: number;
  error: string;
  delta: ImportReloadDelta;
  newProblemKeys: ReadonlySet<string>;
}

const EMPTY_IMPORT_DELTA: ImportReloadDelta = {
  newProblems: [], resolvedProblems: [], changedFiles: 0, addedFiles: 0, removedFiles: 0,
};

function emptyImportReload(): ImportReloadState {
  return { status: "idle", completedAt: 0, durationMs: 0, scenes: 0, error: "", delta: EMPTY_IMPORT_DELTA, newProblemKeys: new Set() };
}

export interface BrowserPreferences {
  afterLaunch: AfterLaunch;
  openTo: OpenTo;
  actorLabels: boolean;
  libraryDetail: "curated" | "full";
  librarySource: "all" | "custom";
  unavailableScenes: UnavailableScenes;
  hideApparel: "-1" | "0" | "1";
  playerInputLock: "-1" | "0" | "1";
  camera: "" | "thirdperson_hold" | "scene_orbit" | "freefly" | "vanity_orbit";
  authorDetails: boolean;
}

export const DEFAULT_PREFERENCES: BrowserPreferences = {
  afterLaunch: "stay",
  openTo: "last",
  actorLabels: true,
  libraryDetail: "curated",
  librarySource: "all",
  unavailableScenes: "ask",
  hideApparel: "-1",
  playerInputLock: "-1",
  camera: "",
  authorDetails: false,
};

export interface WheelEntry {
  scene: string;
  stage: number | null;
  title: string;
  detail: string;
  key: string;
}

export interface WheelState {
  tagPrefix: string;
  target: { token: number; name: string } | null;
  focus: number;
  error: string;
  launching: string;
  received: boolean;
  requested: boolean;
  entries: WheelEntry[];
}

export interface NoticeState {
  kind: "" | "info" | "ok" | "err";
  text: string;
  serial: number;
}

export interface ActorIndicator {
  token: number;
  /** Viewport-normalized (0..1). */
  x: number;
  y: number;
  visible: boolean;
}

export interface PickTarget {
  /** Clicks send the hot marker's token back; there is no second native hit-test. */
  token: number;
  /** Marker anchor above the rendered head, viewport-normalized (0..1). */
  x: number;
  y: number;
  /** Hit-test ellipse: normalized center plus pixel radii. */
  cx: number;
  cy: number;
  rx: number;
  ry: number;
  /** Overlapping ellipses resolve front-most first. */
  depth: number;
}

export interface BrowserState {
  ready: boolean;
  catalog: SceneModel[];
  catalogReceived: boolean;
  library: SceneModel[];
  libraryReceived: boolean;
  wheelCustomized: boolean;
  selectedId: string | null;
  /** Null selects the whole authored emote/scene. */
  selectedStage: number | null;
  cast: CastMember[];
  furniture: FurnitureTarget | null;
  locationMode: LocationMode;
  locationToken: number | null;
  nearbyActors: NearbyTarget[];
  nearbyFurniture: NearbyTarget[];
  pickMode: "actor" | "furniture" | null;
  lastHandle: number;
  lastSceneId: string;
  active: ActiveLaunch[] | null;
  opts: LaunchOptions;
  optsOpen: boolean;
  filters: { search: string; debugMode: boolean };
  plugin: PluginVersion | null;
  /** Native-authoritative role-filter matches for one exact ordered cast. */
  castMatch: { tokens: readonly number[]; ids: ReadonlySet<string> } | null;
  anchorMatch: { token: number; ids: ReadonlySet<string> } | null;
  browseAll: boolean;
  showHidden: boolean;
  browseKind: BrowseKind;
  allSpecies: boolean;
  mode: BrowserMode;
  wheel: WheelState | null;
  preferences: BrowserPreferences;
  settingsOpen: boolean;
  /** Mutually exclusive with `settingsOpen`. */
  importsOpen: boolean;
  imports: ImportFile[];
  importTotals: ImportTotals;
  importsReceived: boolean;
  importsExpanded: ReadonlySet<string>;
  importsFilter: ImportFilter;
  importsSearch: string;
  /** Imports -> View Content widens source/detail filters without mutating preferences. */
  importView: boolean;
  importReload: ImportReloadState;
  lastBrowseMode: BrowseMode;
  minimized: boolean;
  /** Missing keys fall back to selection-driven opening. */
  libOpen: ReadonlyMap<string, boolean>;
  /** Search opens at most one result group, independent of remembered folders. */
  searchGroupOpen: string | null;
  libFull: boolean;
  libCustomOnly: boolean;
  briefFullAnims: boolean;
  markersOpen: boolean;
  stepOpen: { cast: boolean; anchor: boolean };
  seededTokens: ReadonlySet<number>;
  actorIndicators: ActorIndicator[];
  pickTargets: PickTarget[];
  viewVisible: boolean;
  notice: NoticeState;
  visibilitySerial: number;
  worldGeneration: number;
}

export function createInitialState(): BrowserState {
  return {
    ready: false,
    worldGeneration: 0,
    catalog: [],
    catalogReceived: false,
    library: [],
    libraryReceived: false,
    wheelCustomized: false,
    selectedId: null,
    selectedStage: null,
    cast: [PLAYER_CAST],
    furniture: null,
    locationMode: "cast",
    locationToken: null,
    nearbyActors: [],
    nearbyFurniture: [],
    pickMode: null,
    lastHandle: 0,
    lastSceneId: "",
    active: null,
    opts: { hideApparel: "-1", playerInputLock: "-1", camera: "", speed: "1" },
    optsOpen: false,
    filters: { search: "", debugMode: DEFAULT_PREFERENCES.authorDetails },
    plugin: null,
    castMatch: null,
    anchorMatch: null,
    browseAll: false,
    showHidden: false,
    browseKind: "all",
    allSpecies: false,
    mode: "scenes",
    wheel: null,
    preferences: { ...DEFAULT_PREFERENCES },
    settingsOpen: false,
    importsOpen: false,
    imports: [],
    importTotals: { ...EMPTY_IMPORT_TOTALS },
    importsReceived: false,
    importsExpanded: new Set(),
    importsFilter: "attention",
    importsSearch: "",
    importView: false,
    lastBrowseMode: "scenes",
    importReload: emptyImportReload(),
    minimized: false,
    libOpen: new Map(),
    searchGroupOpen: null,
    libFull: false,
    libCustomOnly: false,
    briefFullAnims: false,
    markersOpen: false,
    stepOpen: { cast: true, anchor: true },
    seededTokens: new Set(),
    actorIndicators: [],
    pickTargets: [],
    viewVisible: true,
    notice: { kind: "", text: "", serial: 0 },
    visibilitySerial: 0,
  };
}
