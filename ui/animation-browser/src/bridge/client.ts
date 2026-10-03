import { type BridgeCommand, type NativeMessage } from "./contract";
import type { JsonObject, OSFUIBridge } from "./osfui";

export type NativeMessageListener = (message: NativeMessage) => void;

export interface AnimationBridge {
  readonly standalone: boolean;
  send(command: BridgeCommand, fields?: Record<string, unknown>): void;
  subscribe(listener: NativeMessageListener): () => void;
  dispose(): void;
}

function hasCurrentHelper(bridge: Window["osfui"]): bridge is OSFUIBridge {
  return typeof bridge?.send === "function"
    && typeof bridge.request === "function"
    && typeof bridge.on === "function"
    && typeof bridge.state?.get === "function"
    && typeof bridge.state.on === "function";
}

export class OsfUiBridge implements AnimationBridge {
  readonly standalone = false;
  private readonly listeners = new Set<NativeMessageListener>();
  // The helper can replay readiness/state before the controller's post-paint subscription exists.
  private readonly pending: NativeMessage[] = [];
  private readonly unsubscribers: Array<() => void> = [];

  constructor() {
    const bridge = window.osfui;
    if (!hasCurrentHelper(bridge)) return;

    const events = [
      "osf.animation.version", "osf.animation.catalog.data",
      "osf.animation.library.data", "osf.animation.imports.data",
      "osf.animation.routes.data", "osf.animation.routeInspectResult",
      "osf.animation.imports.reloadResult", "osf.animation.imports.copyResult",
      "osf.animation.wheel.data", "osf.animation.pick", "osf.animation.openTarget",
      "osf.animation.scanResults", "osf.animation.actorIndicators",
      "osf.animation.pickTargets", "osf.animation.anchorMatch", "osf.animation.castMatch",
      "osf.animation.activeScenes", "osf.animation.launchResult",
      "osf.animation.notice", "osf.animation.mode", "ui.visibility",
      "osfui.debug.error",
    ];
    for (const name of events) {
      this.unsubscribers.push(bridge.on(name, (payload) => this.emit({
        type: name === "osfui.debug.error" ? "ui.error" : name,
        payload,
      })));
    }
    this.unsubscribers.push(bridge.state.on("settings", (value) =>
      this.emit({ type: "settings.data", payload: value })));
    this.emit({ type: "runtime.ready", payload: {} });
  }

  send(command: BridgeCommand, fields: Record<string, unknown> = {}): void {
    if (command === "settings.set") {
      void this.setPreference(fields);
      return;
    }
    window.osfui?.send(command, fields as JsonObject);
  }

  private async setPreference(fields: Record<string, unknown>): Promise<void> {
    try {
      const bridge = window.osfui;
      if (!hasCurrentHelper(bridge)) throw new Error("OSF UI bridge is unavailable");
      await bridge.request("osf.animation.settings.set", {
        ...fields,
        value: fields.key === "launch.camera" && fields.value === "" ? "scene" : fields.value,
      } as JsonObject);
    } catch (error) {
      this.emit({
        type: "bridge.requestFailed",
        payload: { command: "settings.set", message: error instanceof Error ? error.message : String(error || "request rejected") },
      });
    }
  }

  subscribe(listener: NativeMessageListener): () => void {
    this.listeners.add(listener);
    if (this.pending.length) {
      const buffered = this.pending.splice(0);
      for (const message of buffered) listener(message);
    }
    return () => this.listeners.delete(listener);
  }

  dispose(): void {
    this.pending.length = 0;
    this.listeners.clear();
    for (const unsubscribe of this.unsubscribers.splice(0)) unsubscribe();
  }

  private emit(message: NativeMessage): void {
    if (this.listeners.size === 0) { this.pending.push(message); return; }
    for (const listener of this.listeners) listener(message);
  }
}

export function hasOsfUiBridge(): boolean {
  return hasCurrentHelper(window.osfui);
}

