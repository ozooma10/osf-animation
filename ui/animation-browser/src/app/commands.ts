import type { BrowseKind, BrowserMode, BrowserPreferences, LocationMode } from "./state";

export interface BrowserCommands {
  refresh(): void;
  setMode(mode: BrowserMode): void;
  selectScene(id: string, stage?: number | null): void;
  setSearch(value: string): void;
  toggleSettings(open?: boolean): void;
  /** Open/close the per-file import report; opening (re)requests it from the engine. */
  toggleImports(open?: boolean): void;
  refreshImports(): void;
  toggleImportFile(path: string, open: boolean): void;
  toggleImportProblemsOnly(): void;
  setImportSearch(value: string): void;
  setPreference<K extends keyof BrowserPreferences>(key: K, value: BrowserPreferences[K]): void;
  toggleBrowseAll(): void;
  setBrowseKind(kind: BrowseKind): void;
  toggleSpecies(): void;
  toggleStep(step: "cast" | "anchor"): void;
  toggleMarkers(): void;
  scan(kind: "actor" | "furniture"): void;
  pick(kind: "actor" | "furniture"): void;
  /** Resolve an armed world-pick click: x/y are viewport-normalized (0..1); the
   *  controller scores them against the polled pick-marker geometry and sends the
   *  hot marker's token — a click with no hot marker is a miss decided locally. */
  pickAt(x: number, y: number, width: number, height: number): void;
  cancelPick(): void;
  toggleActor(token: number): void;
  togglePlayer(): void;
  removeMember(index: number): void;
  moveMember(index: number, delta: -1 | 1): void;
  reorderMember(from: number, to: number, after: boolean): void;
  toggleAnchor(token: number): void;
  clearAnchor(): void;
  selectLocation(mode: LocationMode, token?: number | null): void;
  toggleLibraryGroup(key: string, open: boolean): void;
  toggleLibraryFull(): void;
  toggleLibraryCustomOnly(): void;
  toggleBriefAnimations(): void;
  toggleOptions(): void;
  setOption(field: "strip" | "lock" | "camera" | "speed", value: string): void;
  launch(stage?: number, singleAnimation?: boolean, sceneId?: string): void;
  stop(handle?: number): void;
  stopAll(): void;
  advance(handle?: number): void;
  setMinimized(value: boolean): void;
  toggleWheelEntry(scene: string, stage?: number | null): void;
  moveWheelEntry(scene: string, stage: number | null, direction: -1 | 1): void;
  resetWheel(): void;
  focusWheel(index: number): void;
  pickWheel(index: number): void;
  cancelWheel(): void;
  requestClose(): void;
  orbit(dx: number, dy: number, wheel: number): void;
  openModPage(url: string): void;
}
