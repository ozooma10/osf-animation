import type { SceneModel } from "../model";

export const PLAYER_TOKEN = -1;

export interface CastMember {
  token: number;
  name: string;
  kind?: "player";
  distance?: number | null;
  species: string;
}

export const PLAYER_CAST: CastMember = {
  token: PLAYER_TOKEN,
  name: "Player",
  kind: "player",
  species: "human",
};

export interface NearbyTarget {
  token: number;
  name: string;
  formId: number;
  distance: number | null;
  isActor: boolean;
  species: string;
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

export interface FurnitureTarget {
  token: number;
  name: string;
  distance: number | null;
}

export interface PluginVersion {
  plugin?: string;
  version?: string;
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

export interface BrowserState {
  ready: boolean;
  catalog: SceneModel[];
  catalogReceived: boolean;
  library: SceneModel[];
  libraryReceived: boolean;
  wheelCustomized: boolean;
  selectedId: string | null;
  cast: CastMember[];
  furniture: FurnitureTarget | null;
  nearbyActors: NearbyTarget[];
  nearbyFurniture: NearbyTarget[];
  lastHandle: number;
  lastSceneId: string;
  active: ActiveScene[] | null;
  opts: LaunchOptions;
  optsOpen: boolean;
  filters: { search: string; debugMode: boolean };
  plugin: PluginVersion | null;
  anchorMatch: { token: number; ids: ReadonlySet<string> } | null;
  browseAll: boolean;
  allSpecies: boolean;
  mode: BrowserMode;
  wheel: WheelState | null;
  minimized: boolean;
  libOpen: ReadonlySet<string>;
  scnOpen: ReadonlyMap<string, boolean>;
  libShowAll: boolean;
  libFull: boolean;
  briefFullAnims: boolean;
  markersOpen: boolean;
  stepOpen: { cast: boolean; anchor: boolean };
  seededTokens: ReadonlySet<number>;
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
    cast: [PLAYER_CAST],
    furniture: null,
    nearbyActors: [],
    nearbyFurniture: [],
    lastHandle: 0,
    lastSceneId: "",
    active: null,
    opts: { strip: "-1", lock: "-1", camera: "", speed: "1" },
    optsOpen: false,
    filters: { search: "", debugMode: false },
    plugin: null,
    anchorMatch: null,
    browseAll: false,
    allSpecies: false,
    mode: "scenes",
    wheel: null,
    minimized: false,
    libOpen: new Set(),
    scnOpen: new Map(),
    libShowAll: false,
    libFull: false,
    briefFullAnims: false,
    markersOpen: false,
    stepOpen: { cast: true, anchor: true },
    seededTokens: new Set(),
    notice: { kind: "", text: "", serial: 0 },
    visibilitySerial: 0,
  };
}
