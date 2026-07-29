import { describe, expect, it } from "vitest";
import { browserReducer } from "../src/app/reducer";
import {
  browseVisible,
  filteredLibrary,
  formatDuration,
  formatEstimate,
  isVanillaAnimation,
  libraryFolderTree,
  locationCastChoices,
  playableItems,
  playableVisible,
  validSelection,
  wheelGeometry,
  wheelPool,
} from "../src/app/selectors";
import { PLAYER_CAST, createInitialState } from "../src/app/state";
import { decodePreferences, preferredOpenMode } from "../src/app/settings";
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
    const initial = { ...createInitialState(), cast: [PLAYER_CAST, { token: 7, name: "Sarah", species: "human", sex: "female" }] };
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
    const armed = browserReducer(state, { type: "pick/armed", kind: "actor" });
    const hidden = browserReducer({ ...armed, minimized: true }, { type: "visibility/hidden" });
    expect(hidden).toMatchObject({ mode: "scenes", wheel: null, minimized: false, pickMode: null, actorIndicators: [], viewVisible: false });
  });

  it("tracks native-projected actor indicators and clears them when hidden", () => {
    const projected = browserReducer(createInitialState(), {
      type: "indicators/received",
      items: [{ token: 7, x: 0.7, y: 0.3, visible: true }],
    });
    expect(projected.actorIndicators).toEqual([{ token: 7, x: 0.7, y: 0.3, visible: true }]);
    expect(browserReducer(projected, { type: "visibility/hidden" }).actorIndicators).toEqual([]);
  });

  it("keeps pick targets only while the matching pick mode stays armed", () => {
    const target = { x: 0.6, y: 0.3, cx: 0.6, cy: 0.42, rx: 70, ry: 110 };
    const armed = browserReducer(createInitialState(), { type: "pick/armed", kind: "actor" });
    const polled = browserReducer(armed, { type: "pickTargets/received", slot: "actor", items: [target] });
    expect(polled.pickTargets).toEqual([target]);

    // A stale reply from the other slot must not repaint the marker layer.
    const rearmed = browserReducer(polled, { type: "pick/armed", kind: "furniture" });
    expect(rearmed.pickTargets).toEqual([]);
    expect(browserReducer(rearmed, { type: "pickTargets/received", slot: "actor", items: [target] }).pickTargets).toEqual([]);
    expect(browserReducer(rearmed, { type: "pickTargets/received", slot: "furniture", items: [target] }).pickTargets).toEqual([target]);

    // A late in-flight reply after cancel must not resurrect the marker layer.
    const cancelled = browserReducer(polled, { type: "pick/cancelled" });
    expect(cancelled.pickTargets).toEqual([]);
    expect(browserReducer(cancelled, { type: "pickTargets/received", slot: "actor", items: [target] }).pickTargets).toEqual([]);
  });

  it("honors the after-launch preference when a scene starts", () => {
    const initial = createInitialState();
    const minimized = browserReducer(initial, {
      type: "launch/succeeded", handle: 11, sceneId: "solo", afterLaunch: "minimize",
    });
    const keptOpen = browserReducer(initial, {
      type: "launch/succeeded", handle: 12, sceneId: "pair", afterLaunch: "stay",
    });
    const closed = browserReducer(initial, {
      type: "launch/succeeded", handle: 13, sceneId: "solo", afterLaunch: "close",
    });

    expect(minimized).toMatchObject({ lastHandle: 11, minimized: true });
    expect(keptOpen).toMatchObject({ lastHandle: 12, minimized: false });
    expect(closed).toMatchObject({ lastHandle: 13, minimized: false });
  });

  it("applies synchronized browser and launch preferences", () => {
    const state = browserReducer(createInitialState(), {
      type: "settings/received",
      preferences: { afterLaunch: "stay", libraryDetail: "full", librarySource: "custom", strip: "0", authorDetails: true },
    });
    expect(state.preferences.afterLaunch).toBe("stay");
    expect(state).toMatchObject({ libFull: true, libCustomOnly: true, opts: { strip: "0" }, filters: { debugMode: true } });
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

  it("tracks free-space and furniture location choices", () => {
    const withActor = { ...createInitialState(), cast: [PLAYER_CAST, { token: 7, name: "Sarah", species: "human", sex: "female" }] };
    const atActor = browserReducer(withActor, { type: "location/selected", mode: "actor", token: 7 });
    expect(atActor).toMatchObject({ locationMode: "actor", locationToken: 7 });

    const withoutActor = browserReducer(atActor, { type: "cast/removed", index: 1 });
    expect(withoutActor).toMatchObject({ locationMode: "cast", locationToken: null });

    const atFurniture = browserReducer(withoutActor, { type: "anchor/selected", anchor: { token: 9, name: "Barstool", distance: 2 } });
    expect(atFurniture).toMatchObject({ locationMode: "furniture", locationToken: 9, furniture: { token: 9 } });

    const atPlayer = browserReducer(atFurniture, { type: "location/selected", mode: "player" });
    expect(atPlayer).toMatchObject({ locationMode: "player", locationToken: null, furniture: { token: 9 } });
  });

  it("keeps crew and location expansion under non-header interactions", () => {
    const initial = {
      ...createInitialState(),
      catalog: [solo],
      stepOpen: { cast: true, anchor: true },
    };
    const crewPicked = browserReducer(initial, {
      type: "cast/replaced",
      members: [{ token: 7, name: "Sarah", species: "human", sex: "female" }],
    });
    const locationPicked = browserReducer(crewPicked, {
      type: "anchor/selected",
      anchor: { token: 9, name: "Barstool", distance: 2 },
    });
    const sceneSelected = browserReducer(locationPicked, {
      type: "selection/changed",
      sceneId: solo.id,
    });

    expect(crewPicked.stepOpen).toEqual({ cast: true, anchor: true });
    expect(locationPicked.stepOpen).toEqual({ cast: true, anchor: true });
    expect(sceneSelected.stepOpen).toEqual({ cast: true, anchor: true });
  });

});

describe("browser settings", () => {
  it("decodes typed settings and migrates the legacy Auto-Minimize bool", () => {
    expect(decodePreferences({
      "browser.afterLaunch": "close",
      "browser.rememberBrowsing": false,
      "browser.actorLabels": false,
      "launch.camera": "scene_orbit",
    })).toMatchObject({ afterLaunch: "close", rememberBrowsing: false, actorLabels: false, camera: "scene_orbit" });
    // Absent keys keep their defaults rather than decoding as false.
    expect(decodePreferences({}).actorLabels).toBeUndefined();
    expect(decodePreferences({ "browser.autoMinimize": false })).toMatchObject({ afterLaunch: "stay" });
  });

  it("falls back from Active when no scene is running", () => {
    expect(preferredOpenMode("active", "library", false)).toBe("scenes");
    expect(preferredOpenMode("last", "library", false)).toBe("scenes");
  });
});

describe("browser selectors", () => {
  it("deduplicates equivalent cast location choices", () => {
    const sarah = { token: 7, name: "Sarah", species: "human", sex: "female" };
    const andreja = { token: 8, name: "Andreja", species: "human", sex: "female" };

    const playerFirst = locationCastChoices({
      ...createInitialState(),
      cast: [PLAYER_CAST, sarah, sarah],
    });
    expect(playerFirst.showPlayer).toBe(false);
    expect(playerFirst.actors.map((member) => member.token)).toEqual([7]);

    const npcFirst = locationCastChoices({
      ...createInitialState(),
      cast: [sarah, PLAYER_CAST, andreja, sarah],
    });
    expect(npcFirst.showPlayer).toBe(true);
    expect(npcFirst.actors.map((member) => member.token)).toEqual([8]);
  });
  it("selects a ready action from the unified browse surface", () => {
    const state = { ...createInitialState(), catalog: [solo, pair], catalogReceived: true };
    expect(validSelection(state)).toBe("solo");
    expect(browseVisible(state, pair)).toBe(false);
    expect(validSelection({ ...state, browseAll: true })).toBe("solo");
  });

  it("applies the unavailable-scene visibility preference", () => {
    const state = { ...createInitialState(), catalog: [pair], catalogReceived: true };
    expect(browseVisible(state, pair)).toBe(false);
    expect(browseVisible({ ...state, preferences: { ...state.preferences, unavailableScenes: "show" } }, pair)).toBe(true);
    expect(browseVisible({
      ...state,
      browseAll: true,
      preferences: { ...state.preferences, unavailableScenes: "hide" },
    }, pair)).toBe(false);
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

  it("filters vanilla animations and keeps generated source clips author-only", () => {
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
    expect(validSelection(state)).toBeNull();
    const authorState = { ...state, filters: { ...state.filters, debugMode: true } };
    expect(validSelection(authorState)).toBe(imported.id);
  });

  it("projects library stages as playables while keeping their set as collection metadata", () => {
    const set = normalizeScene({
      id: "vanilla/photomode",
      title: "Vanilla · Photomode / Female",
      pack: "Vanilla",
      folder: "Standing",
      stages: [
        { index: 0, name: "Heroic", tags: ["pose"], clipCount: 1, openEnded: true },
        { index: 1, name: "Wave", clipCount: 1, openEnded: false },
      ],
    });
    const state = {
      ...createInitialState(),
      mode: "scenes" as const,
      library: [set],
      libraryReceived: true,
      libFull: true,
    };
    const items = playableItems(state).filter((item) => playableVisible(state, item));
    expect(items.map((item) => ({ title: item.title, stage: item.stage?.index, collection: item.collection }))).toEqual([
      { title: "Heroic", stage: 0, collection: "Standing / Photomode / Female" },
      { title: "Wave", stage: 1, collection: "Standing / Photomode / Female" },
    ]);
  });

  it("builds case-insensitive nested folders while keeping root clips at the pack level", () => {
    const root = normalizeScene({ id: "root", pack: "Pose Pack", title: "Root" });
    const seated = normalizeScene({ id: "seated", pack: "Pose Pack", folder: "Furniture/Seated", title: "Seated" });
    const leaning = normalizeScene({ id: "leaning", pack: "Pose Pack", folder: "furniture/Leaning", title: "Leaning" });
    const tree = libraryFolderTree("pack:pose pack", [root, seated, leaning]);

    expect(tree.scenes.map((scene) => scene.id)).toEqual(["root"]);
    expect(tree.children).toHaveLength(1);
    expect(tree.children[0]).toMatchObject({ label: "Furniture" });
    expect(tree.children[0].children.map((folder) => folder.label)).toEqual(["Leaning", "Seated"]);
    expect(tree.children[0].children[1].scenes.map((scene) => scene.id)).toEqual(["seated"]);
  });
});
