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
  | "osf.animation.wheel.set";

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
  fields: TFields,
): string {
  return JSON.stringify({ type: "ui.command", payload: { command, ...fields } } satisfies UiCommand<TFields>);
}

export function parseNativeMessage(text: string): NativeMessage | null {
  try {
    const value: unknown = JSON.parse(text);
    if (!value || typeof value !== "object") return null;
    const record = value as Record<string, unknown>;
    if (typeof record.type !== "string") return null;
    return { type: record.type, payload: record.payload };
  } catch {
    return null;
  }
}
