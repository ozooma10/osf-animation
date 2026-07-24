import { describe, expect, it } from "vitest";
import { browserReducer } from "../src/app/reducer";
import {
  browseVisible,
  filteredLibrary,
  formatDuration,
  formatEstimate,
  isVanillaAnimation,
  validSelection,
  wheelGeometry,
  wheelPool,
} from "../src/app/selectors";
import { PLAYER_CAST, createInitialState } from "../src/app/state";
import { normalizeScene } from "../src/model";

const solo = normalizeScene({
  id: "solo",
  title: "Solo",
  actorCount: 1,
  tags: ["player.emote.solo"],
  pinned: 2,
});
const pair = normalizeScene({ id: "pair", title: "Pair", actorCount: 2, pinned: 1 });

describe("browser reducer", () => {
  it("self-heals engine readiness from a catalog reply after a web-view reload", () => {
    const reloaded = createInitialState();
    expect(reloaded.ready).toBe(false);

    // OSF UI's one-shot runtime.ready was delivered to the old page, but every
    // newly mounted page sends catalog.get and OSF Animation answers directly.
    const healed = browserReducer(reloaded, { type: "catalog/received", scenes: [solo] });

    expect(healed).toMatchObject({ ready: true, catalogReceived: true });
  });

  it("keeps cast ordering immutable", () => {
    const initial = { ...createInitialState(), cast: [PLAYER_CAST, { token: 7, name: "Sarah", species: "human" }] };
    const moved = browserReducer(initial, { type: "cast/moved", from: 1, to: 0 });
    expect(moved.cast.map((member) => member.token)).toEqual([7, -1]);
    expect(initial.cast.map((member) => member.token)).toEqual([-1, 7]);
  });

  it("clears transient modes when the host hides the view", () => {
    const state = browserReducer(createInitialState(), {
      type: "wheel/entered",
      tagPrefix: "player.emote.",
      target: null,
    });
    const hidden = browserReducer({ ...state, minimized: true }, { type: "visibility/hidden" });
    expect(hidden).toMatchObject({ mode: "scenes", wheel: null, minimized: false });
  });

  it("distinguishes an explicit wheel from reset defaults", () => {
    const customized = browserReducer(createInitialState(), { type: "wheel/customized", catalog: [solo], library: [] });
    expect(customized.wheelCustomized).toBe(true);
    const reset = browserReducer(customized, { type: "wheel/reset", catalog: [solo], library: [] });
    expect(reset.wheelCustomized).toBe(false);
  });

  it("toggles custom-only animation filtering", () => {
    const state = browserReducer(createInitialState(), { type: "library/customOnly" });
    expect(state.libCustomOnly).toBe(true);
  });
});

describe("browser selectors", () => {
  it("selects only a scene visible in the current lane", () => {
    const state = { ...createInitialState(), catalog: [solo, pair], catalogReceived: true };
    expect(validSelection(state)).toBe("pair");
    expect(browseVisible(state, pair)).toBe(false);
    expect(validSelection({ ...state, browseAll: true })).toBe("pair");
  });

  it("derives default wheel order and caps its geometry", () => {
    const state = { ...createInitialState(), catalog: [solo], catalogReceived: true };
    expect(wheelPool(state).map((entry) => entry.scene)).toEqual(["solo"]);
    expect(wheelGeometry(3)).toEqual({ rx: 150, ry: 140 });
    expect(wheelGeometry(12)).toEqual({ rx: 250, ry: 190 });
  });

  it("formats instrument durations", () => {
    expect(formatDuration(0.2)).toBe("1s");
    expect(formatDuration(150)).toBe("2:30");
    expect(formatEstimate({ estSec: 150, estPartial: true, openEnded: true })).toBe("~2:30+∞");
  });

  it("filters vanilla animations without hiding imported scene clips", () => {
    const vanilla = normalizeScene({ id: "vanilla/common/idle", title: "Vanilla Idle", tags: ["vanilla"], stages: [{ name: "Idle" }] });
    const imported = normalizeScene({ id: "osf.scene-clip/abc", title: "Imported Clip", tags: ["scene.clip"], stages: [{ name: "Pack\\Clip.glb" }] });
    const state = {
      ...createInitialState(),
      mode: "library" as const,
      library: [vanilla, imported],
      libraryReceived: true,
      libCustomOnly: true,
      selectedId: vanilla.id,
    };
    expect(isVanillaAnimation(vanilla)).toBe(true);
    expect(isVanillaAnimation(imported)).toBe(false);
    expect(filteredLibrary(state).map((scene) => scene.id)).toEqual([imported.id]);
    expect(browseVisible(state, vanilla)).toBe(false);
    expect(validSelection(state)).toBe(imported.id);
  });
});
