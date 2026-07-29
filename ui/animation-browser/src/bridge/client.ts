import { encodeCommand, parseNativeMessage, type BridgeCommand, type NativeMessage } from "./contract";

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
  // Messages delivered by the host before any listener has subscribed. The bridge's
  // onMessage handler is installed synchronously in the constructor (render phase), but
  // the controller only calls subscribe() from a post-paint useEffect. The host queues
  // messages for a not-yet-visible view and flushes them at first paint — which lands in
  // that gap. Buffer here and replay on the first subscribe so normal startup becomes
  // ready immediately; a full page reload also self-heals from the catalog response.
  private readonly pending: NativeMessage[] = [];
  private readonly previousHandler = window.osfui?.onMessage;

  constructor() {
    window.osfui ??= {};
    window.osfui.onMessage = (text) => {
      const message = parseNativeMessage(text);
      if (!message) return;
      if (this.listeners.size === 0) { this.pending.push(message); return; }
      for (const listener of this.listeners) listener(message);
    };
  }

  send(command: BridgeCommand, fields: Record<string, unknown> = {}): void {
    window.osfui?.postMessage?.(encodeCommand(command, fields));
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
    if (window.osfui) window.osfui.onMessage = this.previousHandler;
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
  return typeof window.osfui?.postMessage === "function";
}

