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
  private readonly previousHandler = window.osfui?.onMessage;

  constructor() {
    window.osfui ??= {};
    window.osfui.onMessage = (text) => {
      const message = parseNativeMessage(text);
      if (!message) return;
      for (const listener of this.listeners) listener(message);
    };
  }

  send(command: BridgeCommand, fields: Record<string, unknown> = {}): void {
    window.osfui?.postMessage?.(encodeCommand(command, fields));
  }

  subscribe(listener: NativeMessageListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  dispose(): void {
    this.listeners.clear();
    if (window.osfui) window.osfui.onMessage = this.previousHandler;
  }
}

export function hasOsfUiBridge(): boolean {
  return typeof window.osfui?.postMessage === "function";
}

