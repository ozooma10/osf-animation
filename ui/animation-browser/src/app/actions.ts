import type {
  ActiveScene,
  AfterLaunch,
  BrowseKind,
  BrowserMode,
  BrowserPreferences,
  CastMember,
  FurnitureTarget,
  LocationMode,
  NearbyTarget,
  PluginVersion,
  WheelEntry,
} from "./state";
import type { SceneModel } from "../model";

export type BrowserAction =
  | { type: "runtime/ready" }
  | { type: "catalog/requested" }
  | { type: "library/requested" }
  | { type: "plugin/received"; plugin: PluginVersion }
  | { type: "catalog/received"; scenes: SceneModel[] }
  | { type: "library/received"; scenes: SceneModel[] }
  | { type: "active/received"; scenes: ActiveScene[] }
  | { type: "launch/succeeded"; handle: number; sceneId: string; afterLaunch: AfterLaunch }
  | { type: "launch/failed"; error: string }
  | { type: "settings/received"; preferences: Partial<BrowserPreferences> }
  | { type: "settings/open"; open: boolean }
  | { type: "cast/replaced"; members: CastMember[] }
  | { type: "cast/toggled"; member: CastMember }
  | { type: "cast/removed"; index: number }
  | { type: "cast/moved"; from: number; to: number; after?: boolean }
  | { type: "nearby/received"; kind: "actor" | "furniture"; targets: NearbyTarget[] }
  | { type: "indicators/received"; items: import("./state").ActorIndicator[] }
  | { type: "pickTargets/received"; slot: "actor" | "furniture"; items: import("./state").PickTarget[] }
  | { type: "pick/armed"; kind: "actor" | "furniture" }
  | { type: "pick/cancelled" }
  | { type: "anchor/selected"; anchor: FurnitureTarget }
  | { type: "anchor/cleared" }
  | { type: "anchor/matched"; token: number; ids: ReadonlySet<string> }
  | { type: "location/selected"; mode: LocationMode; token?: number | null }
  | { type: "selection/changed"; sceneId: string | null; stage?: number | null }
  | { type: "mode/changed"; mode: BrowserMode }
  | { type: "browser/opened"; mode: Exclude<BrowserMode, "wheel">; resetBrowsing: boolean }
  | { type: "filter/search"; search: string }
  | { type: "filter/species" }
  | { type: "browse/all" }
  | { type: "browse/kind"; kind: BrowseKind }
  | { type: "library/group"; key: string; open: boolean }
  | { type: "brief/fullAnimations" }
  | { type: "brief/options" }
  | { type: "brief/option"; field: "strip" | "lock" | "camera" | "speed"; value: string }
  | { type: "markers/toggled" }
  | { type: "step/toggled"; step: "cast" | "anchor" }
  | { type: "minimized/changed"; minimized: boolean }
  | { type: "scene/stopped"; handle: number }
  | { type: "wheel/entered"; tagPrefix: string; target: { token: number; name: string } | null }
  | { type: "wheel/exited" }
  | { type: "wheel/requested" }
  | { type: "wheel/received"; customized: boolean; entries: WheelEntry[] }
  | { type: "wheel/focused"; focus: number }
  | { type: "wheel/launching"; key: string }
  | { type: "wheel/debug"; entries: WheelEntry[]; customized: boolean; received: boolean; target: { token: number; name: string } | null; error: string }
  | { type: "wheel/customized"; catalog: SceneModel[]; library: SceneModel[] }
  | { type: "wheel/reset"; catalog: SceneModel[]; library: SceneModel[] }
  | { type: "visibility/hidden" }
  | { type: "visibility/shown" }
  | { type: "seeded/remembered"; token: number }
  | { type: "notice/show"; kind: "info" | "ok" | "err"; text: string }
  | { type: "notice/clear"; serial: number };
