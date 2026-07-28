/** Debug-only command surface. Standalone/harness use only — never wired in-game. */

export type VersionDebugState = "none" | "match" | "old";

export interface WheelDebugState {
  count: number;
  pins: boolean;
  target: boolean;
  error: boolean;
  loading: boolean;
}

export interface DevCommands {
  version(state: VersionDebugState): void;
  wheel(state: WheelDebugState | null): void;
}
