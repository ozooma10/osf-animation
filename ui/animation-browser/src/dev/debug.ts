/** Debug-only command surface. Standalone/harness use only — never wired in-game. */

export type VersionDebugState = "none" | "match" | "old";

export interface DevCommands {
  version(state: VersionDebugState): void;
}
