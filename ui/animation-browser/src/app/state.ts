import type { SceneModel } from "../model";

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

export interface ActiveScene {
  handle: number;
  sceneId: string;
  stage: number;
  player: boolean;
  cast: ActiveCastMember[];
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
  strip: "-1" | "0" | "1";
  lock: "-1" | "0" | "1";
  camera: string;
  speed: string;
}

export type BrowserMode = "scenes" | "library" | "active" | "wheel";
export type BrowseMode = Exclude<BrowserMode, "wheel">;
export type BrowseKind = "all" | "animation" | "action" | "scene";
export type AfterLaunch = "minimize" | "stay" | "close";
export type OpenTo = "last" | BrowseMode;
export type UnavailableScenes = "ask" | "show" | "hide";

export interface BrowserPreferences {
  afterLaunch: AfterLaunch;
  openTo: OpenTo;
  rememberBrowsing: boolean;
  libraryDetail: "curated" | "full";
  librarySource: "all" | "custom";
  unavailableScenes: UnavailableScenes;
  strip: "-1" | "0" | "1";
  lock: "-1" | "0" | "1";
  camera: "" | "thirdperson_hold" | "scene_orbit" | "freefly" | "vanity_orbit";
  speed: string;
  authorDetails: boolean;
}

export const DEFAULT_PREFERENCES: BrowserPreferences = {
  afterLaunch: "minimize",
  openTo: "last",
  rememberBrowsing: true,
  libraryDetail: "curated",
  librarySource: "all",
  unavailableScenes: "ask",
  strip: "-1",
  lock: "-1",
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

export interface BrowserState {
  ready: boolean;
  catalog: SceneModel[];
  catalogReceived: boolean;
  library: SceneModel[];
  libraryReceived: boolean;
  wheelCustomized: boolean;
  selectedId: string | null;
  /** Selected library stage. Null selects the whole authored action/scene. */
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
  active: ActiveScene[] | null;
  opts: LaunchOptions;
  optsOpen: boolean;
  filters: { search: string; debugMode: boolean };
  plugin: PluginVersion | null;
  anchorMatch: { token: number; ids: ReadonlySet<string> } | null;
  browseAll: boolean;
  browseKind: BrowseKind;
  allSpecies: boolean;
  mode: BrowserMode;
  wheel: WheelState | null;
  preferences: BrowserPreferences;
  settingsOpen: boolean;
  lastBrowseMode: BrowseMode;
  minimized: boolean;
  libOpen: ReadonlySet<string>;
  scnOpen: ReadonlyMap<string, boolean>;
  libShowAll: boolean;
  libFull: boolean;
  libCustomOnly: boolean;
  briefFullAnims: boolean;
  markersOpen: boolean;
  stepOpen: { cast: boolean; anchor: boolean };
  seededTokens: ReadonlySet<number>;
  actorIndicators: ActorIndicator[];
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
    opts: { strip: "-1", lock: "-1", camera: "", speed: "1" },
    optsOpen: false,
    filters: { search: "", debugMode: DEFAULT_PREFERENCES.authorDetails },
    plugin: null,
    anchorMatch: null,
    browseAll: false,
    browseKind: "all",
    allSpecies: false,
    mode: "scenes",
    wheel: null,
    preferences: { ...DEFAULT_PREFERENCES },
    settingsOpen: false,
    lastBrowseMode: "scenes",
    minimized: false,
    libOpen: new Set(),
    scnOpen: new Map(),
    libShowAll: false,
    libFull: false,
    libCustomOnly: false,
    briefFullAnims: false,
    markersOpen: false,
    stepOpen: { cast: true, anchor: true },
    seededTokens: new Set(),
    actorIndicators: [],
    viewVisible: true,
    notice: { kind: "", text: "", serial: 0 },
    visibilitySerial: 0,
  };
}
