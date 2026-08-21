import { EMPTY_IMPORT_TOTALS, type ImportFile, type ImportReloadDelta, type ImportTotals, type RouteModel, type SceneModel } from "../model";

export const PLAYER_TOKEN = -1;

export interface CastMember {
  token: number;
  name: string;
  kind?: "player";
  distance?: number | null;
  species: string;
  /** "male" / "female", or "" when the actor has no actorbase sex (most creatures). */
  sex: string;
}

export const PLAYER_CAST: CastMember = {
  token: PLAYER_TOKEN,
  name: "Player",
  kind: "player",
  species: "human",
  // The player's sex rides the version push instead (see PluginVersion.playerSex).
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

/** A handle-bearing active item: either a runtime scene or a side-effect-free preview session. */
export interface ActiveLaunch {
  handle: number;
  sceneId: string;
  stage: number;
  player: boolean;
  cast: ActiveCastMember[];
  time: number;
  duration: number;
  speed: number;
  inspection: boolean;
  inspectionKind: "scene" | "route";
  routeId: string;
  transitionId: string;
}

/** Compatibility spelling for callers that consume the frozen `activeScenes` bridge event. */
export type ActiveScene = ActiveLaunch;

export type LocationMode = "cast" | "player" | "actor" | "front" | "furniture";

export interface FurnitureTarget {
  token: number;
  name: string;
  distance: number | null;
}

export interface PluginVersion {
  plugin?: string;
  version?: string;
  /** OSF UI ABI 1.10 raw relative-pointer path registered by OSF Animation. */
  relativePointer?: boolean;
  /** The player character's sex tag, so the permanent player chip can badge M/F too. */
  playerSex?: string;
  ui?: {
    name?: string;
    version?: string;
    tested?: string;
    outdated?: boolean;
    nexusUrl?: string;
  };
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
/** Input accepted at state boundaries while old browser snapshots still say `action`. */
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
  rememberBrowsing: boolean;
  actorLabels: boolean;
  libraryDetail: "curated" | "full";
  librarySource: "all" | "custom";
  unavailableScenes: UnavailableScenes;
  hideApparel: "-1" | "0" | "1";
  playerInputLock: "-1" | "0" | "1";
  camera: "" | "thirdperson_hold" | "scene_orbit" | "freefly" | "vanity_orbit";
  speed: string;
  authorDetails: boolean;
}

export const DEFAULT_PREFERENCES: BrowserPreferences = {
  afterLaunch: "stay",
  openTo: "last",
  rememberBrowsing: true,
  actorLabels: true,
  libraryDetail: "curated",
  librarySource: "all",
  unavailableScenes: "ask",
  hideApparel: "-1",
  playerInputLock: "-1",
  camera: "",
  speed: "1",
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
  /** Viewport-normalized coordinates (0..1), projected by the native render camera. */
  x: number;
  y: number;
  visible: boolean;
}

export interface PickTarget {
  /** Native pick token for the target this marker stands on. A click resolves to
   *  the hot marker and sends this token back, so the marker geometry the user
   *  sees IS the click acceptance — there is no second native hit-test to drift. */
  token: number;
  /** Marker anchor (above the rendered head), viewport-normalized (0..1). */
  x: number;
  y: number;
  /** Hover/click hit-test ellipse: normalized center plus pixel radii
   *  (hottestPickTarget scores against it for both the marker and the click). */
  cx: number;
  cy: number;
  rx: number;
  ry: number;
  /** View depth of the target from the pick camera. Overlapping ellipses resolve
   *  front-most-first — the target the user visually sees under the cursor. */
  depth: number;
}

export interface BrowserState {
  ready: boolean;
  catalog: SceneModel[];
  catalogReceived: boolean;
  library: SceneModel[];
  libraryReceived: boolean;
  routes: RouteModel[];
  routesReceived: boolean;
  routeDebuggerOpen: boolean;
  selectedRouteId: string | null;
  selectedTransitionId: string | null;
  routeSearch: string;
  routeActorToken: number;
  wheelCustomized: boolean;
  selectedId: string | null;
  /** Selected library stage. Null selects the whole authored emote/scene. */
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
  anchorMatch: { token: number; ids: ReadonlySet<string> } | null;
  browseAll: boolean;
  showHidden: boolean;
  browseKind: BrowseKind;
  allSpecies: boolean;
  mode: BrowserMode;
  wheel: WheelState | null;
  preferences: BrowserPreferences;
  settingsOpen: boolean;
  /** The per-file import report panel. Mutually exclusive with `settingsOpen`. */
  importsOpen: boolean;
  imports: ImportFile[];
  importTotals: ImportTotals;
  importsReceived: boolean;
  /** Expanded import rows, keyed by file path. */
  importsExpanded: ReadonlySet<string>;
  /** Outcome filter; attention is the author-first default while All remains one click away. */
  importsFilter: ImportFilter;
  importsSearch: string;
  importReload: ImportReloadState;
  lastBrowseMode: BrowseMode;
  minimized: boolean;
  /** Explicit group disclosure choices. Missing keys fall back to selection-driven opening. */
  libOpen: ReadonlyMap<string, boolean>;
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
}

export function createInitialState(): BrowserState {
  return {
    ready: false,
    catalog: [],
    catalogReceived: false,
    library: [],
    libraryReceived: false,
    routes: [],
    routesReceived: false,
    routeDebuggerOpen: false,
    selectedRouteId: null,
    selectedTransitionId: null,
    routeSearch: "",
    routeActorToken: PLAYER_TOKEN,
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
    lastBrowseMode: "scenes",
    importReload: emptyImportReload(),
    minimized: false,
    libOpen: new Map(),
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
