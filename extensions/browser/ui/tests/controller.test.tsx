// @vitest-environment happy-dom

import { act, cleanup, renderHook } from "@octanejs/testing-library";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { useBrowserController } from "../src/app/controller";

/** Exercise the real bridge, controller, effects and reducer together. Only the host is replaced. */
function nativeHost() {
  const listeners = new Map<string, (payload: unknown) => void>();
  let settings: (payload: unknown) => void = () => {};
  const send = vi.fn((_command: string, _fields?: unknown) => true);
  const request = vi.fn(async (_command: string, _fields?: unknown) => ({}));
  const unsubscribed = vi.fn();
  window.osfui = {
    postMessage() {}, send, request,
    on(name: string, listener: (payload: unknown) => void) {
      listeners.set(name, listener);
      return () => { listeners.delete(name); unsubscribed(name); };
    },
    state: {
      get: () => undefined,
      on(_key: string, listener: (payload: unknown) => void) {
        settings = listener;
        return () => { settings = () => {}; unsubscribed("settings"); };
      },
    },
  } as NonNullable<Window["osfui"]>;
  return {
    send, request, listeners, unsubscribed,
    emit: async (name: string, payload: unknown) => { await act(() => listeners.get(name)?.(payload)); },
    preferences: async (payload: Record<string, unknown>) => { await act(() => settings(payload)); },
    calls: (name: string) => send.mock.calls.filter(([command]) => command === name),
  };
}

async function browser(catalog: unknown[] = [{ id: "solo", actorCount: 1 }], library: unknown[] = []) {
  const host = nativeHost();
  const hook = renderHook(() => useBrowserController());
  await host.emit("osf.animation.world", { generation: 1 });
  await host.emit("osf.animation.catalog.data", catalog);
  await host.emit("osf.animation.library.data", library);
  await host.emit("osf.animation.castMatch", { tokens: [-1], sceneIds: ["solo"] });
  host.send.mockClear();
  return { host, ...hook };
}

beforeEach(() => { vi.useFakeTimers(); vi.setSystemTime(new Date("2026-01-01T00:00:00Z")); });
afterEach(() => { cleanup(); delete window.osfui; vi.useRealTimers(); document.body.className = ""; });

describe("native browser startup and lifetime", () => {
  it.each(["actor", "furniture"] as const)("retains picked cast and %s placement across reopening and rescanning", async (mode) => {
    const { host, result } = await browser([{ id: "duet", actorCount: 2, requiresFurniture: mode === "furniture" }]);
    const actors = [{ token: 7, name: "Sarah", isActor: true }];
    await host.emit("osf.animation.scanResults", { kind: "actor", items: actors });
    await act(() => result.current.commands.toggleActor(7));
    await host.emit("osf.animation.scanResults", { kind: "furniture", items: [{ token: 70, name: "Chair" }] });
    await act(() => result.current.commands.toggleAnchor(70));
    await act(() => result.current.commands.selectLocation(mode, mode === "actor" ? 7 : 70));
    await host.emit("osf.animation.castMatch", { tokens: [-1, 7], sceneIds: ["duet"] });
    await host.emit("osf.animation.anchorMatch", { token: 70, sceneIds: ["duet"] });

    for (let i = 0; i < 2; ++i) {
      await host.emit("ui.visibility", { visible: false });
      await host.emit("ui.visibility", { visible: true });
      await host.emit("osf.animation.world", { generation: 1 });
      await host.emit("osf.animation.openTarget", { token: 7, name: "Sarah" });
      await host.emit("osf.animation.scanResults", { kind: "actor", items: actors });
      expect(result.current.state.cast.map(({ token }) => token)).toEqual([-1, 7]);
      expect(result.current.state.furniture?.token).toBe(70);
      await act(() => result.current.commands.launch(undefined, false, "duet"));
      expect(host.calls("osf.animation.launch").at(-1)?.[1]).toMatchObject({
        castTokens: [-1, 7], location: { mode, token: mode === "actor" ? 7 : 70 },
        ...(mode === "furniture" ? { furnitureToken: 70 } : {}),
      });
    }
    // A scan result for an already-picked actor toggles it off, never appends a duplicate.
    await act(() => result.current.commands.toggleActor(7));
    expect(result.current.state.cast.map(({ token }) => token)).toEqual([-1]);
  });

  it.each([false, true])("clears old-world picks and handles when reset is replayed (hidden delivery: %s)", async (deliverWhileHidden) => {
    const { host, result } = await browser();
    await host.emit("osf.animation.openTarget", { token: 7, name: "Sarah" });
    await host.emit("osf.animation.scanResults", { kind: "actor", items: [{ token: 7, name: "Sarah" }] });
    await host.emit("osf.animation.openTarget", { token: 70, name: "Chair", slot: "furniture" });
    await host.emit("osf.animation.castMatch", { tokens: [7], sceneIds: ["solo"] });
    await host.emit("osf.animation.anchorMatch", { token: 70, sceneIds: ["solo"] });
    await host.emit("osf.animation.launchResult", { ok: true, handle: 41, nodeId: "solo" });
    await host.emit("osf.animation.activeLaunches", { scenes: [{ handle: 41, nodeId: "solo" }] });
    await host.emit("ui.visibility", { visible: false });
    if (deliverWhileHidden) await host.emit("osf.animation.world", { generation: 2 });
    await host.emit("ui.visibility", { visible: true });
    await host.emit("osf.animation.world", { generation: 2 });
    expect(result.current.state).toMatchObject({
      worldGeneration: 2, cast: [{ token: -1 }], furniture: null, locationMode: "cast", locationToken: null,
      nearbyActors: [], nearbyFurniture: [], castMatch: null, anchorMatch: null,
      active: null, lastHandle: 0, lastSceneId: "", wheel: null,
    });
    expect(result.current.state.seededTokens.size).toBe(0);
    await host.emit("osf.animation.openTarget", { token: 8, name: "Sarah" });
    await host.emit("osf.animation.world", { generation: 2 });
    expect(result.current.state.cast.map(({ token }) => token)).toEqual([8]);
  });

  it("requests both catalogs on document initialization and grants gamepad/back routing", async () => {
    const host = nativeHost();
    const { result } = renderHook(() => useBrowserController());
    expect(result.current.standalone).toBe(false);
    expect(host.send).toHaveBeenCalledWith("osfui.gamepadMode", { mode: "buttons" });
    expect(host.send).toHaveBeenCalledWith("osfui.handleBack", { handle: true });
    expect(host.calls("osf.animation.catalog.get").length).toBeGreaterThan(0);
    expect(host.calls("osf.animation.library.get").length).toBeGreaterThan(0);

    await host.emit("osf.animation.catalog.data", []);
    await host.emit("osf.animation.library.data", []);
    host.send.mockClear();
    await act(() => vi.advanceTimersByTime(30_000));
    expect(host.calls("osf.animation.catalog.get")).toHaveLength(0);
    expect(host.calls("osf.animation.library.get")).toHaveLength(0);
    expect(result.current.state).toMatchObject({ ready: true, catalogReceived: true, libraryReceived: true });
  });

  it("bounds missing-engine retries and leaves an actionable persistent error", async () => {
    const host = nativeHost();
    const { result } = renderHook(() => useBrowserController());
    await act(() => vi.advanceTimersByTime(30_000));
    expect(result.current.state.notice.kind).toBe("err");
    expect(result.current.state.notice.text).toContain("No response from OSF Animation");
    const catalogRequests = host.calls("osf.animation.catalog.get").length;
    const libraryRequests = host.calls("osf.animation.library.get").length;
    expect(catalogRequests).toBeGreaterThan(1);
    expect(catalogRequests).toBeLessThanOrEqual(22);
    expect(libraryRequests).toBeLessThanOrEqual(8);
    await act(() => vi.advanceTimersByTime(60_000));
    expect(host.calls("osf.animation.catalog.get")).toHaveLength(catalogRequests);
    expect(host.calls("osf.animation.library.get")).toHaveLength(libraryRequests);
    expect(result.current.state.notice.kind).toBe("err");
  });

  it("unsubscribes every host event and cancels requests, projections and playback polling on unmount", async () => {
    const { host, result, unmount } = await browser();
    await host.emit("osf.animation.openTarget", { token: 7, name: "Sarah" });
    await host.emit("osf.animation.activeLaunches", { scenes: [{ handle: 41, nodeId: "solo" }] });
    await act(() => result.current.commands.setMode("active"));
    await act(() => result.current.commands.refresh());
    await act(() => result.current.commands.toggleImports(true));
    const subscriptions = host.listeners.size + 1;
    unmount();
    expect(host.unsubscribed).toHaveBeenCalledTimes(subscriptions);
    expect(host.listeners.size).toBe(0);
    host.send.mockClear();
    await act(() => vi.advanceTimersByTime(30_000));
    expect(host.send).not.toHaveBeenCalled();
  });

  it("preserves NPC playback when hidden, suspends polls, and resumes them after reopening", async () => {
    const { host, result } = await browser();
    await host.emit("osf.animation.openTarget", { token: 7, name: "Sarah" });
    await host.emit("osf.animation.activeLaunches", { scenes: [{ handle: 41, nodeId: "solo", player: false }] });
    await act(() => result.current.commands.setMode("active"));
    await act(() => vi.advanceTimersByTime(100));
    expect(host.calls("osf.animation.playback.get").length).toBeGreaterThan(0);
    expect(host.calls("osf.animation.projectActors").length).toBeGreaterThan(0);

    await host.emit("ui.visibility", { visible: false });
    host.send.mockClear();
    await act(() => vi.advanceTimersByTime(500));
    expect(result.current.state.active?.map(({ handle }) => handle)).toEqual([41]);
    expect(result.current.state).toMatchObject({ viewVisible: false, minimized: false, pickMode: null, wheel: null });
    expect(host.calls("osf.animation.stop")).toHaveLength(0);
    expect(host.calls("osf.animation.projectActors")).toHaveLength(0);
    expect(host.calls("osf.animation.playback.get")).toHaveLength(0);

    await host.emit("ui.visibility", { visible: true });
    expect(result.current.state.mode).toBe("active");
    expect(host.send).toHaveBeenCalledWith("osf.animation.opened", {});
    expect(host.calls("osf.animation.playback.get").length).toBeGreaterThan(0);
  });
});

describe("native launch contracts", () => {
  it("launches the latest ordered cast, named roles, stage and per-launch options", async () => {
    const { host, result } = await browser([{ id: "duet", actorCount: 2,
      roles: [{ name: "lead" }, { name: "partner" }], stages: [{ name: "intro" }, { name: "loop" }] }]);
    await host.emit("osf.animation.scanResults", { kind: "actor", items: [{ token: 7, name: "Sarah", isActor: true }] });
    await act(() => result.current.commands.toggleActor(7));
    await act(() => result.current.commands.moveMember(1, -1));
    await act(() => result.current.commands.selectScene("duet", 1));
    await act(() => result.current.commands.setOption("hideApparel", "0"));
    await act(() => result.current.commands.setOption("playerInputLock", "1"));
    await act(() => result.current.commands.setOption("speed", "1.5"));
    await act(() => result.current.commands.setOption("camera", "scene_orbit"));
    await act(() => result.current.commands.selectLocation("actor", 7));
    await act(() => result.current.commands.launch());
    expect(host.send).toHaveBeenLastCalledWith("osf.animation.launch", {
      nodeId: "duet", castTokens: [7, -1], roleNames: ["lead", "partner"], singleAnimation: false,
      opts: { hideApparel: 0, playerInputLock: 1, speed: 1.5, camera: "scene_orbit", stage: 1 },
      location: { mode: "actor", token: 7 },
    });
  });

  it("launches library stage zero as a single animation and omits inferred role bindings", async () => {
    const { host, result } = await browser([], [{ id: "clips", actorCount: 1, stages: [{ name: "Wave" }] }]);
    await act(() => result.current.commands.selectScene("clips", 0));
    await act(() => result.current.commands.launch());
    const launch = host.calls("osf.animation.launch")[0]?.[1];
    expect(launch).toMatchObject({ nodeId: "clips", castTokens: [-1], singleAnimation: true });
    expect(launch).not.toHaveProperty("roleNames");
    expect(launch).not.toHaveProperty("opts.stage");
  });

  it("enforces authored furniture placement and ignores stale furniture matches", async () => {
    const { host, result } = await browser([{ id: "chair", actorCount: 1, requiresFurniture: true }]);
    await host.emit("osf.animation.scanResults", { kind: "furniture", items: [{ token: 70, name: "Chair" }] });
    await act(() => result.current.commands.toggleAnchor(70));
    expect(host.send).toHaveBeenCalledWith("osf.animation.anchorMatch", { token: 70 });
    await host.emit("osf.animation.anchorMatch", { token: 71, sceneIds: ["chair"] });
    expect(result.current.state.anchorMatch).toBeNull();
    await host.emit("osf.animation.anchorMatch", { token: 70, sceneIds: ["chair"] });
    expect(result.current.state.anchorMatch?.ids.has("chair")).toBe(true);
    await act(() => result.current.commands.selectLocation("front"));
    await act(() => result.current.commands.launch(undefined, false, "chair"));
    expect(host.send).toHaveBeenLastCalledWith("osf.animation.launch", expect.objectContaining({
      furnitureToken: 70, location: { mode: "furniture", token: 70 },
    }));
  });

  it("keeps follow-actor content attached to its cast despite a front placement choice", async () => {
    const { host, result } = await browser([{ id: "solo", placement: "followActor" }]);
    await act(() => result.current.commands.selectLocation("front"));
    await act(() => result.current.commands.launch());
    expect(host.send).toHaveBeenLastCalledWith("osf.animation.launch", expect.objectContaining({ location: { mode: "cast", token: 0 } }));
  });

  it.each(["stay", "minimize", "close"] as const)("honors the %s post-launch preference only after success", async (afterLaunch) => {
    const { host, result } = await browser();
    await host.preferences({ "browser.afterLaunch": afterLaunch });
    await host.emit("osf.animation.launchResult", { ok: false, error: "Cast is busy" });
    expect(result.current.state.lastHandle).toBe(0);
    expect(result.current.state.notice).toMatchObject({ kind: "err", text: "Cast is busy" });
    expect(host.calls("osf.animation.requestClose")).toHaveLength(0);
    await host.emit("osf.animation.launchResult", { ok: true, handle: 42, nodeId: "solo" });
    expect(result.current.state.lastHandle).toBe(42);
    expect(result.current.state.minimized).toBe(afterLaunch === "minimize");
    expect(host.calls("osf.animation.requestClose")).toHaveLength(afterLaunch === "close" ? 1 : 0);
  });

  it("throttles advance, resolves the sole active handle, and stops every active scene once", async () => {
    const { host, result } = await browser();
    await host.emit("osf.animation.activeLaunches", { scenes: [{ handle: 42, nodeId: "solo" }] });
    await act(() => result.current.commands.advance());
    await act(() => result.current.commands.advance());
    expect(host.calls("osf.animation.advance")).toEqual([["osf.animation.advance", { handle: 42 }]]);
    await act(() => vi.advanceTimersByTime(350));
    await act(() => result.current.commands.advance());
    expect(host.calls("osf.animation.advance")).toHaveLength(2);
    await host.emit("osf.animation.activeLaunches", { scenes: [{ handle: 42 }, { handle: 43 }] });
    await act(() => vi.advanceTimersByTime(350));
    await act(() => result.current.commands.advance());
    expect(host.calls("osf.animation.advance")).toHaveLength(2);
    await act(() => result.current.commands.stopAll());
    await act(() => result.current.commands.stopAll());
    expect(host.calls("osf.animation.stop")).toEqual([
      ["osf.animation.stop", { handle: 42 }], ["osf.animation.stop", { handle: 43 }],
    ]);
    expect(result.current.state.active).toEqual([]);
  });
});

describe("cast, world selection and animation wheel", () => {
  it("seeds an NPC once without toggling it off when the host repeats the opening target", async () => {
    const { host, result } = await browser();
    await host.emit("osf.animation.openTarget", { token: 7, name: "Sarah", sex: "FEMALE" });
    await host.emit("osf.animation.openTarget", { token: 7, name: "Sarah", sex: "FEMALE" });
    expect(result.current.state.cast).toMatchObject([{ token: 7, name: "Sarah", sex: "female" }]);
    expect(host.send).toHaveBeenCalledWith("osf.animation.castMatch", { tokens: [7] });
  });

  it("uses the visible marker token, rejects misses, and drops projections for a stale pick mode", async () => {
    const { host, result } = await browser();
    await act(() => result.current.commands.pick("actor"));
    const marker = { token: 7, x: 0.5, y: 0.4, cx: 0.5, cy: 0.5, rx: 40, ry: 80, depth: 10 };
    await host.emit("osf.animation.pickTargets", { slot: "furniture", items: [marker] });
    expect(result.current.state.pickTargets).toEqual([]);
    await host.emit("osf.animation.pickTargets", { slot: "actor", items: [null, marker, { ...marker, token: 8, rx: 0 }] });
    expect(result.current.state.pickTargets).toHaveLength(1);
    await act(() => result.current.commands.pickAt(0, 0, 1000, 1000));
    expect(host.calls("osf.animation.pickScreen")).toHaveLength(0);
    expect(result.current.state.notice.kind).toBe("err");
    await act(() => result.current.commands.pickAt(0.5, 0.5, 1000, 1000));
    expect(host.send).toHaveBeenLastCalledWith("osf.animation.pickScreen", { slot: "actor", token: 7 });
    await act(() => result.current.commands.cancelPick());
    await host.emit("osf.animation.pickTargets", { slot: "actor", items: [marker] });
    expect(result.current.state.pickTargets).toEqual([]);
  });

  it("keeps actor picking armed for multiple selections but completes a furniture pick", async () => {
    const { host, result } = await browser();
    await act(() => result.current.commands.pick("actor"));
    await host.emit("osf.animation.pick", { valid: false, slot: "actor" });
    expect(result.current.state.pickMode).toBe("actor");
    await host.emit("osf.animation.pick", { valid: true, slot: "actor", token: 7, name: "Sarah" });
    expect(result.current.state.cast.map(({ token }) => token)).toEqual([-1, 7]);
    expect(result.current.state.pickMode).toBe("actor");
    await host.emit("osf.animation.pick", { valid: true, slot: "actor", token: 7, name: "Sarah" });
    expect(result.current.state.cast.map(({ token }) => token)).toEqual([-1]);
    await act(() => result.current.commands.pick("furniture"));
    await host.emit("osf.animation.pick", { valid: true, slot: "furniture", token: 70, name: "Chair" });
    expect(result.current.state).toMatchObject({ pickMode: null, furniture: { token: 70 }, locationMode: "furniture" });
    expect(host.send).toHaveBeenCalledWith("osf.animation.anchorMatch", { token: 70 });
  });

  it("launches a wheel entry on its target once, permits retry after failure, and closes after success", async () => {
    const { host, result } = await browser();
    await host.emit("osf.animation.mode", { mode: "wheel", target: { token: 7, name: "Sarah" } });
    await act(() => vi.advanceTimersByTime(0));
    expect(host.calls("osf.animation.wheel.get")).toHaveLength(1);
    await host.emit("osf.animation.wheel.data", { entries: [null, {}, { scene: "clips", stage: 2, title: "Wave" }] });
    expect(result.current.state.wheel?.entries).toHaveLength(1);
    await act(() => result.current.commands.pickWheel(0));
    await act(() => result.current.commands.pickWheel(0));
    expect(host.calls("osf.animation.launch")).toEqual([
      ["osf.animation.launch", { nodeId: "clips", castTokens: [7], opts: { stage: 2 } }],
    ]);
    await host.emit("osf.animation.launchResult", { ok: false, error: "Actor busy" });
    expect(result.current.state.wheel).toMatchObject({ launching: "", error: "Actor busy" });
    await act(() => result.current.commands.pickWheel(0));
    expect(host.calls("osf.animation.launch")).toHaveLength(2);
    await host.emit("osf.animation.launchResult", { ok: true, handle: 42, nodeId: "clips" });
    expect(host.calls("osf.animation.requestClose")).toHaveLength(1);
    await host.emit("ui.visibility", { visible: false });
    await host.emit("osf.animation.wheel.data", { entries: [{ scene: "late" }] });
    expect(result.current.state.wheel).toBeNull();
  });

  it.each(["hidden", "unmounted"])("cancels a deferred wheel request when the view is %s", async (end) => {
    const { host, unmount } = await browser();
    await host.emit("osf.animation.mode", { mode: "wheel" });
    if (end === "hidden") await host.emit("ui.visibility", { visible: false });
    else unmount();
    host.send.mockClear();
    await act(() => vi.advanceTimersByTime(1));
    expect(host.calls("osf.animation.wheel.get")).toHaveLength(0);
  });
});

describe("import reload and preference failures", () => {
  it("coalesces a pending reload and refreshes both catalogs after a successful report", async () => {
    const { host, result } = await browser();
    await host.emit("osf.animation.imports.data", { files: [{ path: "pack.osf.json", errors: 1, problems: ["Missing clip"] }] });
    await act(() => result.current.commands.reloadImports());
    await act(() => result.current.commands.reloadImports());
    expect(host.calls("osf.animation.imports.reload")).toHaveLength(1);
    await host.emit("osf.animation.imports.reloadResult", {
      ok: true, durationMs: 12, scenes: 1,
      report: { files: [{ path: "pack.osf.json", nodes: 1 }], totals: { files: 1, nodes: 1 } },
    });
    expect(result.current.state.importReload).toMatchObject({ status: "success", durationMs: 12, scenes: 1 });
    expect(result.current.state.importReload.delta.resolvedProblems).toHaveLength(1);
    expect(host.calls("osf.animation.catalog.get")).toHaveLength(1);
    expect(host.calls("osf.animation.library.get")).toHaveLength(1);
  });

  it("preserves the previous import report on reload failure and enables retry", async () => {
    const { host, result } = await browser();
    await host.emit("osf.animation.imports.data", { files: [{ path: "pack.osf.json", nodes: 2 }] });
    await act(() => result.current.commands.reloadImports());
    await host.emit("osf.animation.imports.reloadResult", { ok: false, error: "Reload unavailable", durationMs: 4 });
    expect(result.current.state.imports[0]).toMatchObject({ path: "pack.osf.json", nodes: 2 });
    expect(result.current.state.importReload).toMatchObject({ status: "error", error: "Reload unavailable" });
    expect(host.calls("osf.animation.catalog.get")).toHaveLength(0);
    await act(() => result.current.commands.reloadImports());
    expect(host.calls("osf.animation.imports.reload")).toHaveLength(2);
  });

  it("does not claim to save rejected preferences and routes clipboard failures to the notice", async () => {
    const { host, result } = await browser();
    host.request.mockRejectedValueOnce(new Error("Settings unavailable"));
    await act(() => result.current.commands.setPreference("afterLaunch", "close"));
    expect(host.request).toHaveBeenCalledWith("osf.animation.settings.set", { key: "browser.afterLaunch", value: "close" });
    expect(result.current.state.preferences.afterLaunch).toBe("stay");
    expect(result.current.state.notice).toMatchObject({ kind: "err", text: "That preference could not be saved to OSF Settings." });
    await act(() => result.current.commands.copyImportReport("pack.osf.json"));
    expect(host.send).toHaveBeenLastCalledWith("osf.animation.imports.copy", { path: "pack.osf.json" });
    await host.emit("osf.animation.imports.copyResult", { ok: false, error: "Clipboard unavailable" });
    expect(result.current.state.notice).toMatchObject({ kind: "err", text: "Clipboard unavailable" });
  });
});
