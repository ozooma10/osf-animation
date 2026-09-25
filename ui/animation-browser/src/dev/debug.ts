/** Debug-only command surface. Standalone use only — never wired in-game. */

export interface WheelDebugState {
  count: number;
  pins: boolean;
  target: boolean;
  error: boolean;
  loading: boolean;
}

export interface DevCommands {
  wheel(state: WheelDebugState | null): void;
}
