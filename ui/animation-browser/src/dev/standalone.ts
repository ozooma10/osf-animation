// Everything the controller needs in order to run without a host, kept out of
// the production modules that call it. Reached only through `import.meta.env.DEV`
// guards, so Vite drops this module — and the mock catalog, mock bridge, and
// debug command surface behind it — from the shipped bundle.

import type { BrowserAction } from "../app/actions";
import type { AnimationBridge } from "../bridge/client";
import type { BrowserState } from "../app/state";
import type { DevCommands, VersionDebugState } from "./debug";
import { StandaloneBridge } from "./mock-bridge";
import { MOCK_ACTORS, MOCK_ANCHORS } from "./mock-data";

/** The controller internals the standalone surfaces drive. */
export interface DevRuntime {
  dispatch: (action: BrowserAction) => void;
}

export function createStandaloneBridge(getState: () => BrowserState): AnimationBridge {
  return new StandaloneBridge(getState);
}

/** The nearby targets a host would supply, so pick/scan have something to show. */
export function standaloneNearby(): { actors: unknown[]; anchors: unknown[] } {
  return { actors: MOCK_ACTORS, anchors: MOCK_ANCHORS };
}

/** No-op stand-in so production code can hold a DevCommands without a dev graph. */
export const NO_DEV_COMMANDS: DevCommands = { version: () => undefined };

export function createDebugCommands(runtime: DevRuntime): DevCommands {
  const { dispatch } = runtime;
  return {
    version: (mode: VersionDebugState) => dispatch({
      type: "plugin/received",
      plugin: mode === "none" ? { plugin: "OSF Animation", version: "1.0.0" } : {
        plugin: "OSF Animation",
        version: "1.0.0",
        ui: { name: "OSF UI", version: mode === "old" ? "1.0.0" : "1.1.0", tested: "1.1.0", outdated: mode === "old", nexusUrl: "https://www.nexusmods.com/starfield/mods/17711" },
      },
    }),
  };
}
