// @vitest-environment happy-dom

import { act, cleanup, fireEvent, render, renderHook, screen } from "@octanejs/testing-library";
import { useState } from "octane";
import { afterEach, describe, expect, it, vi } from "vitest";
import { App } from "../src/App";
import type { BrowserCommands } from "../src/app/commands";
import { useBrowserController } from "../src/app/controller";
import { createInitialState } from "../src/app/state";
import { BrowsePanel, sameUnifiedBrowserInputs } from "../src/features/browse/BrowsePanel";
import { Segmented } from "../src/features/shared/Shared";
import { useBrowserInput } from "../src/input/useBrowserInput";
import { normalizeScene } from "../src/model";

afterEach(() => {
  cleanup();
  vi.useRealTimers();
});

const commands = new Proxy({}, {
  get: () => () => undefined,
}) as BrowserCommands;

function InteractiveFields() {
  const [mode, setMode] = useState("scenes");
  const [search, setSearch] = useState("");
  return <>
    <Segmented label="Mode" value={mode} options={[
      { value: "scenes", label: "Scenes" },
      { value: "active", label: "Active" },
    ]} onSelect={setMode}/>
    <input aria-label="Search" value={search}
      onInput={(event) => setSearch(event.currentTarget.value)}/>
    <output>{`${mode}:${search}`}</output>
  </>;
}

describe("Octane view runtime", () => {
  it("renders the real browser shell", () => {
    render(App, { props: { state: createInitialState(), commands } });
    expect(screen.getByText("Animation Browser")).toBeTruthy();
    expect(screen.getByText("waiting for runtime…")).toBeTruthy();
  });

  it("renders an outdated-host warning without advertising a dead external link", () => {
    const state = createInitialState();
    state.ready = true;
    state.plugin = { version: "1.0.0", ui: { version: "1.0.0", tested: "1.1.0", outdated: true, nexusUrl: "https://example.invalid" } };
    render(App, { props: { state, commands } });

    expect(screen.getByText("UPDATE OSF UI")).toBeTruthy();
    expect(screen.queryByRole("link", { name: "UPDATE OSF UI" })).toBeNull();
  });

  it("caps an explicitly opened broad-search group", () => {
    const catalog = Array.from({ length: 150 }, (_, index) => normalizeScene({
      id: `search.scene.${index}`,
      title: `Search Scene ${index}`,
      pack: "Search Pack",
      actorCount: 1,
    }));
    const state = {
      ...createInitialState(),
      ready: true,
      catalog,
      catalogReceived: true,
      libraryReceived: true,
      filters: { search: "search", debugMode: false },
      castMatch: { tokens: [-1], ids: new Set(catalog.map((scene) => scene.id)) },
      searchGroupOpen: "browse:pack:search pack",
    };
    render(BrowsePanel, { props: { state, commands } });

    expect(document.querySelectorAll(".playable-row")).toHaveLength(120);
    expect(screen.getByText("SHOWING 120 OF 150 · REFINE THE SEARCH TO NARROW THIS GROUP")).toBeTruthy();
  });

  it("isolates catalog rendering from projection-only state updates", () => {
    const state = createInitialState();
    expect(sameUnifiedBrowserInputs(state, {
      ...state,
      actorIndicators: [{ token: 7, x: 0.5, y: 0.5, visible: true }],
    })).toBe(true);
    expect(sameUnifiedBrowserInputs(state, {
      ...state,
      filters: { ...state.filters, search: "pose" },
    })).toBe(false);
  });

  it("updates compiled state from native click and input events", () => {
    render(InteractiveFields);
    fireEvent.click(screen.getByRole("radio", { name: "Active" }));
    fireEvent.input(screen.getByRole("textbox", { name: "Search" }), {
      target: { value: "pose" },
    });
    expect(screen.getByText("active:pose")).toBeTruthy();
  });

  it("dispatches through the real memoized browser commands", async () => {
    const { result } = renderHook(() => useBrowserController());
    await act(() => result.current.commands.toggleSettings());
    expect(result.current.state.settingsOpen).toBe(true);
  });

  it("reserves world presses for camera orbit without changing panel selection", () => {
    renderHook(() => useBrowserInput(createInitialState(), commands, false));
    const world = document.createElement("div");
    const panel = document.createElement("div");
    panel.className = "console";
    document.body.append(world, panel);

    const worldDown = new MouseEvent("mousedown", { bubbles: true, cancelable: true, button: 0 });
    const panelDown = new MouseEvent("mousedown", { bubbles: true, cancelable: true, button: 0 });
    world.dispatchEvent(worldDown);
    panel.dispatchEvent(panelDown);

    expect(worldDown.defaultPrevented).toBe(true);
    expect(panelDown.defaultPrevented).toBe(false);
  });

  it("uses paced view deltas for in-game camera drag", () => {
    vi.useFakeTimers();
    const orbit = vi.fn();
    const orbitCommands = new Proxy({ orbit }, {
      get: (target, property) => property === "orbit" ? target.orbit : () => undefined,
    }) as unknown as BrowserCommands;
    renderHook(() => useBrowserInput(createInitialState(), orbitCommands, false));
    const world = document.createElement("div");
    document.body.append(world);

    world.dispatchEvent(new MouseEvent("mousedown", { bubbles: true, cancelable: true, button: 0, clientX: 10, clientY: 20 }));
    document.dispatchEvent(new MouseEvent("mousemove", { bubbles: true, clientX: 12, clientY: 23 }));
    document.dispatchEvent(new MouseEvent("mousemove", { bubbles: true, clientX: 15, clientY: 27 }));
    document.dispatchEvent(new MouseEvent("mousemove", { bubbles: true, clientX: 19, clientY: 32 }));

    vi.advanceTimersByTime(15);
    expect(orbit).not.toHaveBeenCalled();
    vi.advanceTimersByTime(1);
    expect(orbit).toHaveBeenCalledOnce();
    expect(orbit).toHaveBeenLastCalledWith(9, 12, 0);
  });

});
