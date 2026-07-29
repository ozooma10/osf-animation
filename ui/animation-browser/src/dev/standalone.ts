// Everything the controller needs in order to run without a host, kept out of
// the production modules that call it. Reached only through `import.meta.env.DEV`
// guards, so Vite drops this module — and the mock catalog, mock bridge, and
// debug command surface behind it — from the shipped bundle.

import type { BrowserAction } from "../app/actions";
import type { AnimationBridge } from "../bridge/client";
import { isRecord } from "../bridge/contract";
import { WHEEL_MAX, wheelKey } from "../app/selectors";
import type { BrowserState } from "../app/state";
import { normalizeCatalog } from "../model";
import type { DevCommands, VersionDebugState, WheelDebugState } from "./debug";
import { StandaloneBridge } from "./mock-bridge";
import { MOCK_ACTORS, MOCK_ANCHORS, MOCK_CATALOG } from "./mock-data";

/** The controller internals the standalone surfaces drive. */
export interface DevRuntime {
  dispatch: (action: BrowserAction) => void;
  send: (command: "osf.animation.wheel.get", fields?: Record<string, unknown>) => void;
  requestCatalog: (fresh?: boolean) => void;
  stateRef: { current: BrowserState };
}

export function createStandaloneBridge(getState: () => BrowserState): AnimationBridge {
  return new StandaloneBridge(getState);
}

/** The nearby targets a host would supply, so pick/scan have something to show. */
export function standaloneNearby(): { actors: unknown[]; anchors: unknown[] } {
  return { actors: MOCK_ACTORS, anchors: MOCK_ANCHORS };
}

/** `window.mockOpenWheel`, plus the `?wheel` boot parameter that drives it. */
export function installWheelHook(bridge: AnimationBridge): void {
  window.mockOpenWheel = (withTarget = true) => (bridge as StandaloneBridge).openWheel(withTarget);
  const params = new URLSearchParams(location.search);
  if (!params.has("wheel")) return;
  window.setTimeout(() => window.mockOpenWheel?.(params.get("wheel") !== "solo"), 0);
}

/** No-op stand-in so production code can hold a DevCommands without a dev graph. */
export const NO_DEV_COMMANDS: DevCommands = { version: () => undefined, wheel: () => undefined };

export function createDebugCommands(runtime: DevRuntime): DevCommands {
  const { dispatch, send, requestCatalog, stateRef } = runtime;
  return {
    version: (mode: VersionDebugState) => dispatch({
      type: "plugin/received",
      plugin: mode === "none" ? { plugin: "OSF Animation", version: "1.0.0" } : {
        plugin: "OSF Animation",
        version: "1.0.0",
        ui: { name: "OSF UI", version: mode === "old" ? "1.0.0" : "1.1.0", tested: "1.1.0", outdated: mode === "old", nexusUrl: "https://www.nexusmods.com/starfield/mods/17711" },
      },
    }),
    wheel: (config: WheelDebugState | null) => {
      if (!config) {
        requestCatalog(true);
        window.setTimeout(() => { if (stateRef.current.wheel) send("osf.animation.wheel.get", { tagPrefix: stateRef.current.wheel.tagPrefix }); }, 80);
        return;
      }
      const base = MOCK_CATALOG.filter((value) => isRecord(value) && Array.isArray(value.tags) && value.tags.some((tag: unknown) => String(tag).startsWith("player.emote.")));
      const raw = Array.from({ length: config.count }, (_, index) => {
        const source = base[index % base.length] as Record<string, unknown>;
        const cycle = Math.floor(index / base.length) + 1;
        return { ...source, id: cycle === 1 ? source.id : `${source.id}.${index}`, title: cycle === 1 ? source.title : `${source.title} ${cycle}`, wheelCustomized: config.pins, pinned: config.pins && index < 3 ? 3 - index : 0 };
      });
      const scenes = normalizeCatalog(raw);
      dispatch({ type: "catalog/received", scenes });
      if (!stateRef.current.wheel) return;
      const ordered = config.pins ? scenes.slice(0, 3).reverse() : scenes;
      const entries = ordered.slice(0, WHEEL_MAX).map((scene) => ({ scene: scene.id, stage: null, title: scene.title, detail: scene.title, key: wheelKey(scene.id, null) }));
      dispatch({ type: "wheel/debug", entries, customized: config.pins, received: !config.loading, target: config.target ? { token: 601, name: "Sarah Morgan" } : null, error: config.error ? "No room in front of the actor (debug)." : "" });
    },
  };
}
