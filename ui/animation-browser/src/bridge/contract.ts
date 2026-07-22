export type BridgeCommand =
  | "osf.animation.advance"
  | "osf.animation.anchorMatch"
  | "osf.animation.catalog.get"
  | "osf.animation.closed"
  | "osf.animation.launch"
  | "osf.animation.library.get"
  | "osf.animation.opened"
  | "osf.animation.orbit"
  | "osf.animation.pickCrosshair"
  | "osf.animation.requestClose"
  | "osf.animation.scanNearby"
  | "osf.animation.stop"
  | "osf.animation.wheel.get"
  | "osf.animation.wheel.set"
  | "osfui.gamepadRaw"
  | "osfui.openModPage";

export interface UiCommand<TFields extends Record<string, unknown> = Record<string, unknown>> {
  type: "ui.command";
  payload: TFields & { command: BridgeCommand };
}

export interface NativeMessage {
  type: string;
  payload?: unknown;
}

export function encodeCommand<TFields extends Record<string, unknown>>(
  command: BridgeCommand,
  fields: TFields = {} as TFields,
): string {
  return JSON.stringify({ type: "ui.command", payload: { command, ...fields } } satisfies UiCommand<TFields>);
}

export function parseNativeMessage(text: string): NativeMessage | null {
  try {
    const value: unknown = JSON.parse(text);
    if (!isRecord(value) || typeof value.type !== "string") return null;
    return { type: value.type, payload: value.payload };
  } catch {
    return null;
  }
}

export function isRecord(value: unknown): value is Record<string, any> {
  return !!value && typeof value === "object" && !Array.isArray(value);
}

export function stringField(record: Record<string, any>, key: string, fallback = ""): string {
  return record[key] == null ? fallback : String(record[key]);
}

export function numberField(record: Record<string, any>, key: string, fallback = 0): number {
  const value = Number(record[key]);
  return Number.isFinite(value) ? value : fallback;
}

