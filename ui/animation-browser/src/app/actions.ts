import type {
  ActiveScene,
  BrowserMode,
  CastMember,
  FurnitureTarget,
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
  | { type: "launch/succeeded"; handle: number; sceneId: string }
  | { type: "launch/failed"; error: string }
  | { type: "cast/replaced"; members: CastMember[] }
  | { type: "cast/toggled"; member: CastMember }
  | { type: "cast/removed"; index: number }
  | { type: "cast/moved"; from: number; to: number; after?: boolean }
  | { type: "nearby/received"; kind: "actor" | "furniture"; targets: NearbyTarget[] }
  | { type: "anchor/selected"; anchor: FurnitureTarget }
  | { type: "anchor/cleared" }
  | { type: "anchor/matched"; token: number; ids: ReadonlySet<string> }
  | { type: "selection/changed"; sceneId: string | null }
  | { type: "mode/changed"; mode: BrowserMode }
  | { type: "filter/search"; search: string }
  | { type: "filter/debug" }
  | { type: "filter/species" }
  | { type: "browse/all" }
  | { type: "library/showAll" }
  | { type: "library/full" }
  | { type: "library/customOnly" }
  | { type: "library/group"; key: string }
  | { type: "scene/group"; key: string; open: boolean }
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
  | { type: "wheel/error"; error: string }
  | { type: "wheel/debug"; entries: WheelEntry[]; customized: boolean; received: boolean; target: { token: number; name: string } | null; error: string }
  | { type: "wheel/customized"; catalog: SceneModel[]; library: SceneModel[] }
  | { type: "wheel/reset"; catalog: SceneModel[]; library: SceneModel[] }
  | { type: "visibility/hidden" }
  | { type: "seeded/remembered"; token: number }
  | { type: "notice/show"; kind: "info" | "ok" | "err"; text: string }
  | { type: "notice/clear"; serial: number };
