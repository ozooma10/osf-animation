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

  it("delivers a complete multi-megabyte library through the original event before subscription", () => {
    const bridge = new OsfUiBridge();
    const payload = { scenes: Array.from({ length: 800 }, (_, i) => ({
      id: `library-${i}`, title: "Café🎬".repeat(100),
      stages: Array.from({ length: 8 }, (_, stage) => ({ index: stage, name: "animation".repeat(20) })),
    })) };
    const text = JSON.stringify(payload), encoder = new TextEncoder();
    const bytes = encoder.encode(text).length;
    expect(bytes).toBeGreaterThan(1024 * 1024);
    const chunks: string[] = [];
    // The native serializer splits on UTF-8 boundaries. Use small complete-code-point
    // slices here so the test also catches using UTF-16 length as the wire byte count.
    for (let start = 0; start < text.length;) {
      let end = Math.min(start + 16_000, text.length);
      if (end < text.length && text.charCodeAt(end) >= 0xDC00 && text.charCodeAt(end) <= 0xDFFF) end--;
      chunks.push(text.slice(start, end)); start = end;
    }
    for (let index = chunks.length - 1; index >= 0; index--) {
      deliver("osf.animation.library.data", { osfAnimationTransfer: 1, id: "1", index, count: chunks.length, bytes, data: chunks[index] });
    }
    const received: NativeMessage[] = [];
    bridge.subscribe((message) => received.push(message));
    expect(received).toEqual([
      { type: "runtime.ready", payload: {} },
      { type: "osf.animation.library.data", payload },
    ]);
    bridge.dispose();
  });

  it("reports malformed transfers as bridge errors without emitting a partial catalog", () => {
    const bridge = new OsfUiBridge(), received: NativeMessage[] = [];
    bridge.subscribe((message) => received.push(message));
    deliver("osf.animation.catalog.data", { osfAnimationTransfer: 1, id: "1", index: 0, count: 2, bytes: 99, data: "{" });
    deliver("osf.animation.catalog.data", { osfAnimationTransfer: 1, id: "1", index: 1, count: 3, bytes: 99, data: "}" });
    expect(received.map((message) => message.type)).toEqual(["runtime.ready", "ui.error"]);
    expect(received[1].payload).toMatchObject({ message: expect.stringContaining("metadata changed") });
    bridge.dispose();
  });

  it("cancels partial data when hidden and removes transfer listeners on dispose", () => {
    const bridge = new OsfUiBridge(), received: NativeMessage[] = [];
    bridge.subscribe((message) => received.push(message));
    const first = { osfAnimationTransfer: 1, id: "1", index: 0, count: 2, bytes: 13, data: '{"scenes":' };
    const last = { ...first, index: 1, data: "[]}" };
    deliver("osf.animation.library.data", first);
    deliver("ui.visibility", { visible: false });
    deliver("osf.animation.library.data", last);
    expect(received.map((message) => message.type)).toEqual(["runtime.ready", "ui.visibility"]);
    deliver("osf.animation.library.data", { ...first, id: "2" });
    deliver("osf.animation.library.data", { ...last, id: "2" });
    expect(received.at(-1)).toEqual({ type: "osf.animation.library.data", payload: { scenes: [] } });
    bridge.dispose();
    expect(eventHandlers.size).toBe(0);
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

  it("rejects removed preferences and invalid values without publishing a changed snapshot", () => {
    vi.useFakeTimers();
    (globalThis as any).window = { setTimeout };
    const bridge = new StandaloneBridge(createInitialState);
    const received: NativeMessage[] = [];
    bridge.subscribe((message) => received.push(message));
    received.length = 0;
    for (const [key, value] of [["launch.speed", "2"], ["browser.rememberBrowsing", false],
      ["browser.actorLabels", "false"], ["launch.camera", "invalid"]]) {
      bridge.send("settings.set", { key, value });
    }
    vi.runAllTimers();
    expect(received).toHaveLength(4);
    expect(received.every((message) => message.type === "bridge.requestFailed")).toBe(true);
    bridge.dispose();
  });
});
