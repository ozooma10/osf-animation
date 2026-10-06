import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { hasOsfUiBridge, OsfUiBridge } from "../src/bridge/client";
import type { NativeMessage } from "../src/bridge/contract";
import { createInitialState } from "../src/app/state";
import { StandaloneBridge } from "../src/dev/mock-bridge";

const eventHandlers = new Map<string, (payload: unknown) => void>();

function deliver(type: string, payload: unknown): void {
  eventHandlers.get(type)?.(payload);
}

function currentHelper(): Record<string, unknown> {
  return {
    send() { return true; },
    request() { return Promise.resolve({}); },
    on(name: string, fn: (payload: unknown) => void) { eventHandlers.set(name, fn); return () => eventHandlers.delete(name); },
    state: { get() { return undefined; }, on() { return () => {}; } },
  };
}

function installHelper(): void {
  eventHandlers.clear();
  (globalThis as any).window = { osfui: currentHelper() };
}

describe("OsfUiBridge subscription race", () => {
  beforeEach(() => installHelper());
  afterEach(() => { delete (globalThis as any).window; });

  it("replays helper presence detected before the first subscribe", () => {
    const bridge = new OsfUiBridge();

    const received: NativeMessage[] = [];
    bridge.subscribe((m) => received.push(m));

    expect(received).toEqual([{ type: "runtime.ready", payload: {} }]);
  });

  it("delivers live messages to an already-subscribed listener", () => {
    const bridge = new OsfUiBridge();
    const received: NativeMessage[] = [];
    bridge.subscribe((m) => received.push(m));
    deliver("osf.animation.notice", { text: "hi" });
    expect(received).toEqual([
      { type: "runtime.ready", payload: {} },
      { type: "osf.animation.notice", payload: { text: "hi" } },
    ]);
  });

  it("replays Animation-owned settings before the controller subscribes", () => {
    const snapshot = { "browser.openTo": "active" };
    let stateKey = "";
    (window.osfui as any).state.on = (key: string, listener: (value: unknown) => void) => {
      stateKey = key;
      listener(snapshot);
      return () => {};
    };
    const bridge = new OsfUiBridge();
    const received: NativeMessage[] = [];
    bridge.subscribe((message) => received.push(message));
    expect(stateKey).toBe("settings");
    expect(received[0]).toEqual({ type: "settings.data", payload: snapshot });
  });

  it("saves the scene camera default through Animation's own endpoint", () => {
    let request: unknown;
    (window.osfui as any).request = (name: string, payload: unknown) => {
      request = { name, payload };
      return Promise.resolve({});
    };
    const bridge = new OsfUiBridge();
    bridge.send("settings.set", { key: "launch.camera", value: "" });
    expect(request).toEqual({
      name: "osf.animation.settings.set",
      payload: { key: "launch.camera", value: "scene" },
    });
  });

  it("surfaces rejected request commands instead of silently discarding them", async () => {
    (window.osfui as any).request = () => Promise.reject(new Error("write refused"));
    const bridge = new OsfUiBridge();
    const received: NativeMessage[] = [];
    bridge.subscribe((message) => received.push(message));

    bridge.send("settings.set", { key: "browser.openTo", value: "active" });
    await Promise.resolve();
    await Promise.resolve();

    expect(received[received.length - 1]).toEqual({
      type: "bridge.requestFailed",
      payload: { command: "settings.set", message: "write refused" },
    });
  });
});

describe("bridge environment detection", () => {
  afterEach(() => { delete (globalThis as any).window; });

  it("uses the native bridge when the current helper is complete", () => {
    (globalThis as any).window = { osfui: currentHelper() };
    expect(hasOsfUiBridge()).toBe(true);
  });

  it.each([
    ["send", { ...currentHelper(), send: undefined }],
    ["request", { ...currentHelper(), request: undefined }],
    ["on", { ...currentHelper(), on: undefined }],
    ["state.get", { ...currentHelper(), state: { get: undefined, on() { return () => {}; } } }],
    ["state.on", { ...currentHelper(), state: { get() { return undefined; }, on: undefined } }],
  ])("rejects an incomplete helper missing %s", (_operation, osfui) => {
    (globalThis as any).window = { osfui };
    expect(hasOsfUiBridge()).toBe(false);
  });

  it("uses the native bridge even when the view is nested in a frame", () => {
    // Frame topology must not decide this: a nested game view is still the game.
    (globalThis as any).window = { parent: {}, osfui: currentHelper() };
    expect(hasOsfUiBridge()).toBe(true);
  });

  it("detects a standalone page without an injected bridge", () => {
    (globalThis as any).window = {};
    expect(hasOsfUiBridge()).toBe(false);
  });
});

describe("standalone settings state", () => {
  afterEach(() => { vi.useRealTimers(); delete (globalThis as any).window; });

  it("replays preferences on subscribe and publishes complete snapshots after writes", () => {
    vi.useFakeTimers();
    (globalThis as any).window = { setTimeout };
    const bridge = new StandaloneBridge(createInitialState);
    const received: NativeMessage[] = [];
    bridge.subscribe((message) => received.push(message));
    expect(received[0]).toMatchObject({
      type: "settings.data",
      payload: { "browser.openTo": "last", "launch.camera": "scene" },
    });
    bridge.send("settings.set", { key: "browser.openTo", value: "active" });
    bridge.send("settings.set", { key: "browser.authorDetails", value: true });
    vi.runAllTimers();
    expect(received[received.length - 1]).toMatchObject({
      type: "settings.data",
      payload: { "browser.openTo": "active", "browser.authorDetails": true, "launch.camera": "scene" },
    });
    bridge.dispose();
  });
});
