import {
  type AfterLaunch,
  type BrowserPreferences,
  type BrowseMode,
  type OpenTo,
  type UnavailableScenes,
} from "./state";

export const PREFERENCE_KEYS: { [K in keyof BrowserPreferences]: string } = {
  afterLaunch: "browser.afterLaunch",
  openTo: "browser.openTo",
  rememberBrowsing: "browser.rememberBrowsing",
  actorLabels: "browser.actorLabels",
  libraryDetail: "browser.libraryDetail",
  librarySource: "browser.librarySource",
  unavailableScenes: "browser.unavailableScenes",
  strip: "launch.strip",
  lock: "launch.lock",
  camera: "launch.camera",
  speed: "launch.speed",
  authorDetails: "browser.authorDetails",
};

const oneOf = <T extends string>(value: unknown, choices: readonly T[]): T | undefined =>
  typeof value === "string" && choices.includes(value as T) ? value as T : undefined;

export function decodePreferences(values: Record<string, unknown>): Partial<BrowserPreferences> {
  const preferences: Partial<BrowserPreferences> = {};
  const afterLaunch = oneOf<AfterLaunch>(values[PREFERENCE_KEYS.afterLaunch], ["minimize", "stay", "close"]);
  const openTo = oneOf<OpenTo>(values[PREFERENCE_KEYS.openTo], ["last", "scenes", "library", "active"]);
  const libraryDetail = oneOf<BrowserPreferences["libraryDetail"]>(values[PREFERENCE_KEYS.libraryDetail], ["curated", "full"]);
  const librarySource = oneOf<BrowserPreferences["librarySource"]>(values[PREFERENCE_KEYS.librarySource], ["all", "custom"]);
  const unavailableScenes = oneOf<UnavailableScenes>(values[PREFERENCE_KEYS.unavailableScenes], ["ask", "show", "hide"]);
  const strip = oneOf<BrowserPreferences["strip"]>(values[PREFERENCE_KEYS.strip], ["-1", "0", "1"]);
  const lock = oneOf<BrowserPreferences["lock"]>(values[PREFERENCE_KEYS.lock], ["-1", "0", "1"]);
  const camera = oneOf<BrowserPreferences["camera"]>(values[PREFERENCE_KEYS.camera], ["", "thirdperson_hold", "scene_orbit", "freefly", "vanity_orbit"]);
  const speed = oneOf(values[PREFERENCE_KEYS.speed], ["0.5", "0.75", "1", "1.25", "1.5", "2"]);

  if (afterLaunch) preferences.afterLaunch = afterLaunch;
  // Compatibility with the old boolean setting if a host still returns it.
  else if (typeof values["browser.autoMinimize"] === "boolean") preferences.afterLaunch = values["browser.autoMinimize"] ? "minimize" : "stay";
  // "library" was the old separate Animations tab. Both old browse lanes now
  // open the unified browser; retain decoding so existing profiles migrate.
  if (openTo) preferences.openTo = openTo === "library" ? "scenes" : openTo;
  if (typeof values[PREFERENCE_KEYS.rememberBrowsing] === "boolean") preferences.rememberBrowsing = values[PREFERENCE_KEYS.rememberBrowsing] as boolean;
  if (typeof values[PREFERENCE_KEYS.actorLabels] === "boolean") preferences.actorLabels = values[PREFERENCE_KEYS.actorLabels] as boolean;
  if (libraryDetail) preferences.libraryDetail = libraryDetail;
  if (librarySource) preferences.librarySource = librarySource;
  if (unavailableScenes) preferences.unavailableScenes = unavailableScenes;
  if (strip) preferences.strip = strip;
  if (lock) preferences.lock = lock;
  if (camera !== undefined) preferences.camera = camera;
  if (speed) preferences.speed = speed;
  if (typeof values[PREFERENCE_KEYS.authorDetails] === "boolean") preferences.authorDetails = values[PREFERENCE_KEYS.authorDetails] as boolean;
  return preferences;
}

export function preferenceFromChange(key: unknown, value: unknown): Partial<BrowserPreferences> {
  if (typeof key !== "string") return {};
  return decodePreferences({ [key]: value });
}

export function preferredOpenMode(openTo: OpenTo, last: BrowseMode, hasActive: boolean): BrowseMode {
  const requested = openTo === "last" ? last : openTo;
  if (requested === "library") return "scenes";
  return requested === "active" && !hasActive ? "scenes" : requested;
}
