import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { hasOsfUiBridge, OsfUiBridge } from "../src/bridge/client";
import type { NativeMessage } from "../src/bridge/contract";

const eventHandlers = new Map<string, (payload: unknown) => void>();

// OsfUiBridge only touches window.osfui; stub the public helper so these run in node.
function deliver(type: string, payload: unknown): void {
  eventHandlers.get(type)?.(payload);
}

function installHelper(ready: Promise<Record<string, unknown>> = new Promise(() => undefined)): void {
  eventHandlers.clear();
  (globalThis as any).window = { osfui: {
    send() { return true; },
    request() { return Promise.resolve({}); },
    ready,
    on(name: string, fn: (payload: unknown) => void) { eventHandlers.set(name, fn); return () => eventHandlers.delete(name); },
    state: { on() { return () => {}; } },
  } };
}

describe("OsfUiBridge subscription race", () => {
  beforeEach(() => installHelper());
  afterEach(() => { delete (globalThis as any).window; });

  it("replays helper readiness resolved before the first subscribe", async () => {
    installHelper(Promise.resolve({ bridgeVersion: "2.0" }));
    const bridge = new OsfUiBridge();
    await Promise.resolve();

    const received: NativeMessage[] = [];
    bridge.subscribe((m) => received.push(m));

    expect(received).toEqual([{ type: "runtime.ready", payload: { bridgeVersion: "2.0", protocol: "2.0" } }]);
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
    (globalThis as any).window = { osfui: { send() {} } };
    expect(hasOsfUiBridge()).toBe(true);
  });

  it("uses the native bridge even when the view is nested in a frame", () => {
    // Frame topology must not decide this: a nested game view is still the game.
    (globalThis as any).window = { parent: {}, osfui: { send() {} } };
    expect(hasOsfUiBridge()).toBe(true);
  });

  it("keeps the stateful simulator inside the OSF UI authoring harness", () => {
    (globalThis as any).window = {
      parent: {},
      __osfuiHarness: { meta: {} },
      osfui: { send() {} },
    };
    expect(hasOsfUiBridge()).toBe(false);
  });
});
