// @vitest-environment happy-dom

import { act, cleanup, renderHook } from "@octanejs/testing-library";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import type { BrowserCommands } from "../src/app/commands";
import { createInitialState, type BrowserState } from "../src/app/state";
import { useBrowserInput } from "../src/input/useBrowserInput";

function inputCommands() {
  const calls = {
    toggleSettings: vi.fn(), toggleImports: vi.fn(), cancelPick: vi.fn(), cancelWheel: vi.fn(),
    requestClose: vi.fn(), focusWheel: vi.fn(), pickWheel: vi.fn(), advance: vi.fn(),
    orbit: vi.fn(), pickAt: vi.fn(), beginOrbitCapture: vi.fn(), endOrbitCapture: vi.fn(),
  };
  return { calls, commands: calls as unknown as BrowserCommands };
}

function wheelState(focus = 0): BrowserState {
  return {
    ...createInitialState(),
    wheel: {
      tagPrefix: "player.emote.", target: null, focus, error: "", launching: "", received: true, requested: true,
      entries: ["wave", "point", "sit"].map((scene) => ({ scene, stage: null, key: scene, title: scene, detail: "" })),
    },
  };
}

function keyboard(key: string, extra: KeyboardEventInit = {}) {
  const event = new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true, ...extra });
  document.dispatchEvent(event);
  return event;
}

function mouse(type: string, x: number, y: number, target: EventTarget = document) {
  target.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, button: 0, clientX: x, clientY: y }));
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => { cleanup(); vi.useRealTimers(); document.body.replaceChildren(); document.body.className = ""; });

describe("keyboard and gamepad routing", () => {
  it.each([
    ["settings", { ...wheelState(), settingsOpen: true, importsOpen: true, pickMode: "actor" as const }, "toggleSettings"],
    ["imports", { ...wheelState(), importsOpen: true, pickMode: "actor" as const }, "toggleImports"],
    ["world picking", { ...wheelState(), pickMode: "actor" as const }, "cancelPick"],
    ["wheel", wheelState(), "cancelWheel"],
    ["browser", createInitialState(), "requestClose"],
  ] as const)("Escape handles %s before closing the browser", (_name, state, expected) => {
    const { calls, commands } = inputCommands();
    renderHook(() => useBrowserInput(state, commands, false));
    keyboard("Escape");
    expect(calls[expected]).toHaveBeenCalledOnce();
    for (const name of ["toggleSettings", "toggleImports", "cancelPick", "cancelWheel", "requestClose"] as const) {
      if (name !== expected) expect(calls[name]).not.toHaveBeenCalled();
    }
  });

  it.each([
    ["ArrowLeft", 0, 2], ["ArrowUp", 0, 2], ["ArrowRight", 2, 0], ["ArrowDown", 2, 0],
  ])("wraps the wheel on %s without requesting a launch", (key, focus, expected) => {
    const { calls, commands } = inputCommands();
    renderHook(() => useBrowserInput(wheelState(Number(focus)), commands, false));
    expect(keyboard(String(key)).defaultPrevented).toBe(true);
    expect(calls.focusWheel).toHaveBeenCalledWith(expected);
    expect(calls.pickWheel).not.toHaveBeenCalled();
    expect(document.body.classList.contains("nav-kb")).toBe(true);
  });

  it.each(["Enter", " ", "Spacebar"])("activates the focused wheel entry with %s", (key) => {
    const { calls, commands } = inputCommands();
    renderHook(() => useBrowserInput(wheelState(1), commands, false));
    expect(keyboard(key).defaultPrevented).toBe(true);
    expect(calls.pickWheel).toHaveBeenCalledWith(1);
    expect(calls.advance).not.toHaveBeenCalled();
  });

  it("leaves an empty wheel safe to navigate and cancels it with right-click", () => {
    const { calls, commands } = inputCommands();
    const state = wheelState();
    state.wheel!.entries = [];
    renderHook(() => useBrowserInput(state, commands, false));
    keyboard("ArrowDown");
    expect(calls.focusWheel).not.toHaveBeenCalled();
    const context = new MouseEvent("contextmenu", { bubbles: true, cancelable: true });
    document.dispatchEvent(context);
    expect(context.defaultPrevented).toBe(true);
    expect(calls.cancelWheel).toHaveBeenCalledOnce();
  });

  it.each(["input", "button", "select", "a"])("does not steal Space from a focused %s", (tag) => {
    const { calls, commands } = inputCommands();
    renderHook(() => useBrowserInput({ ...createInitialState(), lastHandle: 42 }, commands, false));
    const element = document.createElement(tag);
    if (tag === "a") element.setAttribute("href", "#test");
    document.body.append(element);
    element.focus();
    expect(document.activeElement).toBe(element);
    expect(keyboard(" ").defaultPrevented).toBe(false);
    expect(calls.advance).not.toHaveBeenCalled();
  });

  it("advances the scene from the world but respects modified shortcuts and text cursor keys", () => {
    const { calls, commands } = inputCommands();
    renderHook(() => useBrowserInput({ ...createInitialState(), lastHandle: 42 }, commands, false));
    expect(keyboard(" ").defaultPrevented).toBe(true);
    expect(calls.advance).toHaveBeenCalledOnce();
    keyboard("Escape", { ctrlKey: true });
    expect(calls.requestClose).not.toHaveBeenCalled();
    const search = document.createElement("input");
    search.type = "search";
    document.body.append(search);
    search.focus();
    expect(keyboard("ArrowLeft").defaultPrevented).toBe(false);
    expect(keyboard("ArrowRight").defaultPrevented).toBe(false);
  });

  it("removes handlers when unmounted and does not duplicate them after rerender", async () => {
    const { calls, commands } = inputCommands();
    const { rerender, unmount } = renderHook(({ state }) => useBrowserInput(state, commands, false), {
      initialProps: { state: createInitialState() },
    });
    await act(() => rerender({ state: { ...createInitialState(), settingsOpen: true } }));
    keyboard("Escape");
    expect(calls.toggleSettings).toHaveBeenCalledOnce();
    expect(calls.requestClose).not.toHaveBeenCalled();
    unmount();
    keyboard("Escape");
    expect(calls.toggleSettings).toHaveBeenCalledOnce();
  });
});

describe("world picking and orbit lifetime", () => {
  it.each([[3, 4, true], [6, 0, false]])("distinguishes a %s by %s pixel pick movement from orbit", (dx, dy, pick) => {
    const { calls, commands } = inputCommands();
    const state = { ...createInitialState(), pickMode: "actor" as const, plugin: { relativePointer: true } };
    renderHook(() => useBrowserInput(state, commands, false));
    mouse("mousedown", 100, 100, document.body);
    mouse("mousemove", 100 + Number(dx), 100 + Number(dy));
    mouse("mouseup", 100 + Number(dx), 100 + Number(dy));
    vi.advanceTimersByTime(16);
    expect(calls.beginOrbitCapture).not.toHaveBeenCalled();
    if (pick) {
      expect(calls.pickAt).toHaveBeenCalledWith((100 + Number(dx)) / innerWidth, (100 + Number(dy)) / innerHeight, innerWidth, innerHeight);
      expect(calls.orbit).not.toHaveBeenCalled();
    } else {
      expect(calls.pickAt).not.toHaveBeenCalled();
      expect(calls.orbit).toHaveBeenCalledWith(dx, dy, 0);
    }
  });

  it("drops queued orbit motion on hide and never replays it when the view reopens", async () => {
    const { calls, commands } = inputCommands();
    const state = createInitialState();
    const { rerender } = renderHook(({ state }) => useBrowserInput(state, commands, false), { initialProps: { state } });
    mouse("mousedown", 10, 20, document.body);
    mouse("mousemove", 30, 50);
    await act(() => rerender({ state: { ...state, viewVisible: false, visibilitySerial: 1 } }));
    vi.advanceTimersByTime(20);
    expect(calls.orbit).not.toHaveBeenCalled();
    await act(() => rerender({ state: { ...state, visibilitySerial: 1 } }));
    mouse("mousemove", 40, 60);
    vi.advanceTimersByTime(20);
    expect(calls.orbit).not.toHaveBeenCalled();
    mouse("mousedown", 40, 60, document.body);
    mouse("mousemove", 42, 63);
    vi.advanceTimersByTime(16);
    expect(calls.orbit).toHaveBeenCalledExactlyOnceWith(2, 3, 0);
  });

  it.each(["hide", "unmount"])("releases native pointer capture exactly once on %s", async (end) => {
    const { calls, commands } = inputCommands();
    const state = { ...createInitialState(), plugin: { relativePointer: true } };
    const { rerender, unmount } = renderHook(({ state }) => useBrowserInput(state, commands, false), { initialProps: { state } });
    mouse("mousedown", 10, 20, document.body);
    expect(calls.beginOrbitCapture).toHaveBeenCalledOnce();
    if (end === "hide") await act(() => rerender({ state: { ...state, viewVisible: false, visibilitySerial: 1 } }));
    else unmount();
    mouse("mouseup", 20, 30);
    expect(calls.endOrbitCapture).toHaveBeenCalledOnce();
    expect(calls.orbit).not.toHaveBeenCalled();
  });

  it.each(["console", "brief", "livebar"])("leaves scrolling and presses inside the %s panel to its controls", (className) => {
    const { calls, commands } = inputCommands();
    renderHook(() => useBrowserInput(createInitialState(), commands, false));
    const panel = document.createElement("div");
    panel.className = className;
    const button = document.createElement("button");
    panel.append(button);
    document.body.append(panel);
    mouse("mousedown", 10, 20, button);
    mouse("mousemove", 30, 50);
    const wheel = new WheelEvent("wheel", { bubbles: true, cancelable: true, deltaY: 100 });
    button.dispatchEvent(wheel);
    vi.advanceTimersByTime(20);
    expect(wheel.defaultPrevented).toBe(false);
    expect(calls.orbit).not.toHaveBeenCalled();
  });

  it("coalesces world scroll steps and cancels pending input on unmount", () => {
    const { calls, commands } = inputCommands();
    const { unmount } = renderHook(() => useBrowserInput(createInitialState(), commands, false));
    for (const deltaY of [120, 120, -120]) document.body.dispatchEvent(new WheelEvent("wheel", { bubbles: true, cancelable: true, deltaY }));
    vi.advanceTimersByTime(16);
    expect(calls.orbit).toHaveBeenCalledExactlyOnceWith(0, 0, 1);
    document.body.dispatchEvent(new WheelEvent("wheel", { bubbles: true, deltaY: 120 }));
    unmount();
    vi.advanceTimersByTime(16);
    expect(calls.orbit).toHaveBeenCalledOnce();
  });
});
