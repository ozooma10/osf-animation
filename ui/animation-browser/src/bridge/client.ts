import { type BridgeCommand, type NativeMessage } from "./contract";

export type NativeMessageListener = (message: NativeMessage) => void;

export interface AnimationBridge {
  readonly standalone: boolean;
  send(command: BridgeCommand, fields?: Record<string, unknown>): void;
  subscribe(listener: NativeMessageListener): () => void;
  dispose(): void;
}

export class OsfUiBridge implements AnimationBridge {
  readonly standalone = false;
  private readonly listeners = new Set<NativeMessageListener>();
  // The shared helper can replay readiness/state while the controller is still
  // mounting. Hold those adapter messages until its post-paint subscription exists.
  private readonly pending: NativeMessage[] = [];
  private readonly unsubscribers: Array<() => void> = [];

  constructor() {
    const bridge = window.osfui;
    const events = [
      "osf.animation.version", "settings.changed", "osf.animation.catalog.data",
      "osf.animation.library.data", "osf.animation.imports.data",
      "osf.animation.routes.data", "osf.animation.routeInspectResult",
      "osf.animation.imports.reloadResult", "osf.animation.imports.copyResult",
      "osf.animation.wheel.data", "osf.animation.pick", "osf.animation.openTarget",
      "osf.animation.scanResults", "osf.animation.actorIndicators",
      "osf.animation.pickTargets", "osf.animation.anchorMatch",
      "osf.animation.activeScenes", "osf.animation.launchResult",
      "osf.animation.notice", "osf.animation.mode", "ui.visibility", "ui.gamepad",
      "osfui.debug.error",
    ];
    for (const name of events) {
      const off = bridge?.on?.(name, (payload) => this.emit({
        type: name === "osfui.debug.error" ? "ui.error" : name,
        payload,
      }));
      if (off) this.unsubscribers.push(off);
    }
    const offSettings = bridge?.state?.on?.("osfui/settings", (value) =>
      this.emit({ type: "settings.data", payload: value }));
    if (offSettings) this.unsubscribers.push(offSettings);
    bridge?.ready?.then((runtime) => this.emit({
      type: "runtime.ready",
      payload: { ...runtime, protocol: runtime.bridgeVersion },
    })).catch(() => undefined);
  }

  send(command: BridgeCommand, fields: Record<string, unknown> = {}): void {
    if (command === "settings.get") return; // osfui/settings state replays on subscribe
    if (command === "settings.set" || command === "osfui.openModPage") {
      void window.osfui?.request?.(command, fields).catch(() => undefined);
      return;
    }
    window.osfui?.send?.(command, fields);
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
    this.listeners.clear();
    for (const unsubscribe of this.unsubscribers.splice(0)) unsubscribe();
  }

  private emit(message: NativeMessage): void {
    if (this.listeners.size === 0) { this.pending.push(message); return; }
    for (const listener of this.listeners) listener(message);
  }
}

export function hasOsfUiBridge(): boolean {
  // The OSF UI authoring harness injects the production-shaped bridge into its
  // iframe. Keep using this view's richer stateful simulator there; the generic
  // harness still owns framing, sizing, transparency, visibility, and HMR.
  // Ask the harness directly rather than inferring it from frame nesting — a
  // shipped view must never decide "this is only a simulation" from topology it
  // does not control.
  if (window.__osfuiHarness) return false;
  return typeof window.osfui?.send === "function";
}

