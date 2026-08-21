// @vitest-environment happy-dom

import { act, cleanup, fireEvent, render, renderHook, screen } from "@octanejs/testing-library";
import { useState } from "octane";
import { afterEach, describe, expect, it } from "vitest";
import { App } from "../src/App";
import type { BrowserCommands } from "../src/app/commands";
import { useBrowserController } from "../src/app/controller";
import { createInitialState } from "../src/app/state";
import { Segmented } from "../src/features/shared/Shared";
import { useBrowserInput } from "../src/input/useBrowserInput";

afterEach(cleanup);

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
});
