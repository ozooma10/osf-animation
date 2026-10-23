import {
  type AfterLaunch,
  type BrowserPreferences,
  type BrowseMode,
} from "./state";

export const PREFERENCE_KEYS: { [K in keyof BrowserPreferences]: string } = {
  afterLaunch: "browser.afterLaunch",
  actorLabels: "browser.actorLabels",
  libraryDetail: "browser.libraryDetail",
  librarySource: "browser.librarySource",
  hideApparel: "launch.strip",
  playerInputLock: "launch.lock",
  camera: "launch.camera",
  authorDetails: "browser.authorDetails",
};

const oneOf = <T extends string>(value: unknown, choices: readonly T[]): T | undefined =>
  typeof value === "string" && choices.includes(value as T) ? value as T : undefined;

const bool = (value: unknown): boolean | undefined => typeof value === "boolean" ? value : undefined;

export function decodePreferences(values: Record<string, unknown>): Partial<BrowserPreferences> {
  const preferences: Partial<BrowserPreferences> = {};
  const afterLaunch = oneOf<AfterLaunch>(values[PREFERENCE_KEYS.afterLaunch], ["minimize", "stay", "close"]);
  const libraryDetail = oneOf<BrowserPreferences["libraryDetail"]>(values[PREFERENCE_KEYS.libraryDetail], ["curated", "full"]);
  const librarySource = oneOf<BrowserPreferences["librarySource"]>(values[PREFERENCE_KEYS.librarySource], ["all", "custom"]);
  const hideApparel = oneOf<BrowserPreferences["hideApparel"]>(values[PREFERENCE_KEYS.hideApparel], ["-1", "0", "1"]);
  const playerInputLock = oneOf<BrowserPreferences["playerInputLock"]>(values[PREFERENCE_KEYS.playerInputLock], ["-1", "0", "1"]);
  const camera = oneOf<BrowserPreferences["camera"]>(values[PREFERENCE_KEYS.camera] === "scene" ? "" : values[PREFERENCE_KEYS.camera], ["", "thirdperson_hold", "scene_orbit", "freefly", "vanity_orbit"]);
  const actorLabels = bool(values[PREFERENCE_KEYS.actorLabels]);
  const authorDetails = bool(values[PREFERENCE_KEYS.authorDetails]);

  if (afterLaunch) preferences.afterLaunch = afterLaunch;
  if (actorLabels !== undefined) preferences.actorLabels = actorLabels;
  if (libraryDetail) preferences.libraryDetail = libraryDetail;
  if (librarySource) preferences.librarySource = librarySource;
  if (hideApparel) preferences.hideApparel = hideApparel;
  if (playerInputLock) preferences.playerInputLock = playerInputLock;
  if (camera !== undefined) preferences.camera = camera;
  if (authorDetails !== undefined) preferences.authorDetails = authorDetails;
  return preferences;
}

// The browser always reopens where it was left; Active falls back to Browse when nothing runs.
export function preferredOpenMode(last: BrowseMode, hasActive: boolean): BrowseMode {
  return last === "active" && !hasActive ? "scenes" : last;
}
