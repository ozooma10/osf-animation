import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { hasOsfUiBridge, OsfUiBridge } from "../src/bridge/client";
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
    // which lands before the controller's post-paint useEffect subscribes. Buffering keeps
    // the normal startup immediate; manual page reload recovery is covered in app.test.ts.
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

describe("bridge environment detection", () => {
  afterEach(() => { delete (globalThis as any).window; });

  it("uses the native bridge whenever the host injected one", () => {
    (globalThis as any).window = { osfui: { postMessage() {} } };
    expect(hasOsfUiBridge()).toBe(true);
  });

  it("uses the native bridge even when the view is nested in a frame", () => {
    // Frame topology must not decide this: a nested game view is still the game.
    (globalThis as any).window = { parent: {}, osfui: { postMessage() {} } };
    expect(hasOsfUiBridge()).toBe(true);
  });

  it("keeps the stateful simulator inside the OSF UI authoring harness", () => {
    (globalThis as any).window = {
      parent: {},
      __osfuiHarness: { meta: {} },
      osfui: { postMessage() {} },
    };
    expect(hasOsfUiBridge()).toBe(false);
  });
});
