export type BridgeCommand =
  | "osf.animation.advance"
  | "osf.animation.anchorMatch"
  | "osf.animation.catalog.get"
  | "osf.animation.closed"
  | "osf.animation.imports.get"
  | "osf.animation.imports.copy"
  | "osf.animation.imports.reload"
  | "osf.animation.launch"
  | "osf.animation.library.get"
  | "osf.animation.opened"
  | "osf.animation.orbit"
  | "osf.animation.pickScreen"
  | "osf.animation.playback.get"
  | "osf.animation.playback.set"
  | "osf.animation.routes.get"
  | "osf.animation.route.inspect"
  | "osf.animation.projectActors"
  | "osf.animation.projectPickables"
  | "osf.animation.requestClose"
  | "osf.animation.scanNearby"
  | "osf.animation.stop"
  | "osf.animation.wheel.get"
  | "osf.animation.wheel.set"
  | "osfui.gamepadMode"
  | "osfui.handleBack"
  | "osfui.openModPage"
  | "settings.get"
  | "settings.set";

export interface NativeMessage {
  type: string;
  payload?: unknown;
}

export function isRecord(value: unknown): value is Record<string, any> {
  return !!value && typeof value === "object" && !Array.isArray(value);
}
