import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { OsfUiBridge } from "../src/bridge/client";
import type { NativeMessage } from "../src/bridge/contract";

// OsfUiBridge only touches window.osfui; stub a bare window so these run in the node env.
function deliver(type: string, payload: unknown): void {
  (globalThis as any).window.osfui.onMessage(JSON.stringify({ type, payload }));
}

describe("OsfUiBridge subscription race", () => {
  beforeEach(() => { (globalThis as any).window = {}; });
  afterEach(() => { delete (globalThis as any).window; });

  it("replays a runtime.ready flushed before the first subscribe", () => {
    const bridge = new OsfUiBridge();
    // The host queues messages for a not-yet-visible view and flushes them at first paint —
    // which lands before the controller's post-paint useEffect subscribes. Without buffering
    // this message is dropped and the view wedges at "Engine Offline".
    deliver("runtime.ready", { bridgeVersion: "1.3" });

    const received: NativeMessage[] = [];
    bridge.subscribe((m) => received.push(m));

    expect(received).toEqual([{ type: "runtime.ready", payload: { bridgeVersion: "1.3" } }]);
  });

  it("delivers live messages to an already-subscribed listener", () => {
    const bridge = new OsfUiBridge();
    const received: NativeMessage[] = [];
    bridge.subscribe((m) => received.push(m));
    deliver("osf.animation.notice", { text: "hi" });
    expect(received).toEqual([{ type: "osf.animation.notice", payload: { text: "hi" } }]);
  });
});
