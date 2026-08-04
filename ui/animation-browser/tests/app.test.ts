import { describe, expect, it } from "vitest";
import { browserReducer } from "../src/app/reducer";
import {
  evaluateForState,
  comparePlayableGroupKeys,
  comparePlayableItems,
  filteredLibrary,
  formatDuration,
  formatEstimate,
  hiddenSceneCount,
  hottestPickTarget,
  isVanillaAnimation,
  labeledFurniture,
  showUnavailable,
  locationCastChoices,
  playableItems,
  playableGroupOpen,
  playableVisible,
  importGroups,
  importOutcome,
  importOutcomeCounts,
  importResult,
  readableAnimationName,
  validSelection,
  importSeverity,
  isGeneratedSceneClip,
  visibleImports,
  formatBytes,
  formatMillis,
  wheelGeometry,
  wheelPool,
} from "../src/app/selectors";
import { PLAYER_CAST, createInitialState } from "../src/app/state";
import { decodePreferences, preferredOpenMode } from "../src/app/settings";
import { normalizeActive } from "../src/app/controller";
import { normalizeImportReport, normalizeScene, type ImportFile } from "../src/model";

const solo = normalizeScene({
  id: "solo",
  title: "Solo",
  actorCount: 1,
  tags: ["player.emote.solo"],
  pinned: 2,
});
const pair = normalizeScene({ id: "pair", title: "Pair", actorCount: 2, pinned: 1 });

describe("browser reducer", () => {
  it("normalizes authoritative playback clock fields for the active timeline", () => {
    expect(normalizeActive([{ handle: 7, sceneId: "solo", stage: 2, time: 1.25, duration: 3.5, speed: 0, inspection: true, cast: [] }])[0])
      .toMatchObject({ handle: 7, stage: 2, time: 1.25, duration: 3.5, speed: 0, inspection: true });
    expect(normalizeActive([{ handle: 8, sceneId: "legacy", cast: [] }])[0])
      .toMatchObject({ time: 0, duration: 0, speed: 0, inspection: false });
    expect(normalizeActive([{ handle: -1, sceneId: "preview", inspection: true, cast: [] }])[0])
      .toMatchObject({ handle: -1, sceneId: "preview", inspection: true });
  });

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

  it("labels selected furniture under the world-label display policy", () => {
    const furniture = { token: 41, name: "Industrial Chair", distance: 3 };
    const visible = { ...createInitialState(), furniture, viewVisible: true };
    expect(labeledFurniture(visible)).toEqual(furniture);
    expect(labeledFurniture({ ...visible, minimized: true })).toBeNull();
    expect(labeledFurniture({ ...visible, preferences: { ...visible.preferences, actorLabels: false } })).toBeNull();
  });

  it("keeps pick targets only while the matching pick mode stays armed", () => {
    const target = { token: 41, x: 0.6, y: 0.3, cx: 0.6, cy: 0.42, rx: 70, ry: 110, depth: 5 };
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

  it("opens an inspected scene in Active without minimizing", () => {
    const inspected = browserReducer({ ...createInitialState(), mode: "scenes" }, {
      type: "launch/succeeded", handle: 14, sceneId: "solo", afterLaunch: "minimize", inspect: true,
    });
    expect(inspected).toMatchObject({ lastHandle: 14, lastSceneId: "solo", mode: "active", minimized: false });
  });

  it("keeps a minimized preview minimized when it switches stages", () => {
    const preview = {
      ...createInitialState(),
      minimized: true,
      lastHandle: -1,
      lastSceneId: "solo",
      active: [{ handle: -1, sceneId: "solo", stage: 0, player: true, cast: [], time: 0, duration: 2, speed: 0, inspection: true }],
    };
    const restaged = browserReducer(preview, {
      type: "launch/succeeded", handle: -2, sceneId: "solo", afterLaunch: "stay", inspect: true,
    });
    expect(restaged).toMatchObject({ lastHandle: -2, minimized: true });

    // A fresh inspection launched from a minimized RUNNING scene still opens the timeline.
    const fromLive = browserReducer({ ...preview, active: [{ ...preview.active[0], inspection: false }] }, {
      type: "launch/succeeded", handle: -3, sceneId: "solo", afterLaunch: "stay", inspect: true,
    });
    expect(fromLive).toMatchObject({ lastHandle: -3, minimized: false });
  });

  it("applies synchronized browser and launch preferences", () => {
    const state = browserReducer(createInitialState(), {
      type: "settings/received",
      preferences: { afterLaunch: "stay", libraryDetail: "full", librarySource: "custom", strip: "0", authorDetails: true },
    });
    expect(state.preferences.afterLaunch).toBe("stay");
    expect(state).toMatchObject({ libFull: true, libCustomOnly: true, opts: { strip: "0" }, filters: { debugMode: true } });
    expect(state.showHidden).toBe(false);
  });


  it("distinguishes an explicit wheel from reset defaults", () => {
    const customized = browserReducer(createInitialState(), { type: "wheel/customized", catalog: [solo], library: [] });
    expect(customized.wheelCustomized).toBe(true);
    const reset = browserReducer(customized, { type: "wheel/reset", catalog: [solo], library: [] });
    expect(reset.wheelCustomized).toBe(false);
  });

  it("toggles custom-only animation filtering via the settings payload", () => {
    const state = browserReducer(createInitialState(), { type: "settings/received", preferences: { librarySource: "custom" } });
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
  it("resolves the hot pick target with the shared marker/click scoring", () => {
    // 1000x500 viewport. One scoring function serves the hover marker AND the
    // click resolution, so these expectations pin the world-pick acceptance.
    const near = { token: 1, x: 0.3, y: 0.2, cx: 0.3, cy: 0.4, rx: 60, ry: 100, depth: 5 };
    const far = { token: 2, x: 0.8, y: 0.3, cx: 0.8, cy: 0.5, rx: 60, ry: 100, depth: 20 };
    // Dead center of `near`'s ellipse.
    expect(hottestPickTarget([near, far], 300, 200, 1000, 500)?.token).toBe(1);
    // Inside `far`'s ellipse, outside `near`'s.
    expect(hottestPickTarget([near, far], 810, 260, 1000, 500)?.token).toBe(2);
    // Just inside the horizontal edge of `near` (dx = 59/60 < 1).
    expect(hottestPickTarget([near], 359, 200, 1000, 500)?.token).toBe(1);
    // Just outside every ellipse → no hot target, which the click treats as a miss.
    expect(hottestPickTarget([near, far], 500, 480, 1000, 500)).toBeNull();
    expect(hottestPickTarget([], 300, 200, 1000, 500)).toBeNull();
  });

  it("prefers the front-most target when acceptance ellipses overlap", () => {
    // Same ellipse, different depths: the user visually sees the closer actor, so
    // the closer one must win even when the deeper one's center is nearer the cursor.
    const front = { token: 1, x: 0.5, y: 0.2, cx: 0.52, cy: 0.4, rx: 80, ry: 120, depth: 4 };
    const behind = { token: 2, x: 0.5, y: 0.2, cx: 0.5, cy: 0.4, rx: 80, ry: 120, depth: 18 };
    expect(hottestPickTarget([behind, front], 500, 200, 1000, 500)?.token).toBe(1);
    expect(hottestPickTarget([front, behind], 500, 200, 1000, 500)?.token).toBe(1);
    // At effectively the same depth (inside the 5% relative band) the better
    // ellipse score wins instead.
    const twin = { ...behind, depth: 4.1 };
    expect(hottestPickTarget([front, twin], 500, 200, 1000, 500)?.token).toBe(2);
  });

  it("selects a ready action from the unified browse surface", () => {
    const state = { ...createInitialState(), catalog: [solo, pair], catalogReceived: true };
    expect(validSelection(state)).toBe("solo");
    // pair is unplayable for the default cast — the live split (UnifiedBrowser) buckets by gaps
    expect(evaluateForState(state, pair).gaps).toBeGreaterThan(0);
    expect(validSelection({ ...state, browseAll: true })).toBe("solo");
  });

  it("prioritizes photomode poses ahead of actions and cleans exported names", () => {
    const photomode = normalizeScene({
      id: "vanilla/photomode/female",
      title: "Vanilla · Photomode / Female",
      tags: ["vanilla", "photomode"],
      actorCount: 1,
      stages: [{ index: 0, name: "ArmsCrossed_Pose", tags: ["pose"], clipCount: 1, openEnded: true }],
    });
    const state = {
      ...createInitialState(),
      catalog: [solo],
      catalogReceived: true,
      library: [{ ...photomode, library: true }],
      libraryReceived: true,
    };
    const items = playableItems(state).filter((item) => playableVisible(state, item))
      .sort((a, b) => comparePlayableItems(state, a, b));

    expect(items.map((item) => [item.kind, item.title])).toEqual([
      ["animation", "Arms Crossed"],
      ["action", "Solo"],
    ]);
    expect(readableAnimationName("LooseAnim_FormalApplause01")).toBe("Formal Applause 01");
    expect(readableAnimationName("SitOnGround_Pose", true)).toBe("Sit On Ground");
  });

  it("puts vanilla photomode first and raw Scene fragments last", () => {
    const keys = ["custom-pack", "vanilla-scenes", "vanilla-idles", "vanilla-photomode"];
    expect(keys.sort(comparePlayableGroupKeys)).toEqual([
      "vanilla-photomode",
      "custom-pack",
      "vanilla-idles",
      "vanilla-scenes",
    ]);
  });

  it("allows a selected animation group to be explicitly collapsed", () => {
    const state = createInitialState();
    expect(playableGroupOpen(state, "browse:pack:vanilla", true)).toBe(true);

    const collapsed = browserReducer(state, {
      type: "library/group",
      key: "browse:pack:vanilla",
      open: false,
    });
    expect(playableGroupOpen(collapsed, "browse:pack:vanilla", true)).toBe(false);
    expect(playableGroupOpen(collapsed, "browse:pack:other", true)).toBe(true);
  });

  it("applies the unavailable-scene visibility preference", () => {
    const state = { ...createInitialState(), catalog: [pair], catalogReceived: true };
    // default preference is "ask" with the reveal collapsed
    expect(showUnavailable(state)).toBe(false);
    expect(showUnavailable({ ...state, browseAll: true })).toBe(true);
    expect(showUnavailable({ ...state, preferences: { ...state.preferences, unavailableScenes: "show" } })).toBe(true);
    expect(showUnavailable({
      ...state,
      browseAll: true,
      preferences: { ...state.preferences, unavailableScenes: "hide" },
    })).toBe(false);
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

  it("filters vanilla animations and keeps generated source clips hidden until requested", () => {
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
    // the custom-only filter removes vanilla from the playable surface entirely
    expect(playableItems(state).some((item) => item.scene.id === vanilla.id)).toBe(false);
    expect(validSelection(state)).toBeNull();
    const authorState = { ...state, filters: { ...state.filters, debugMode: true } };
    expect(validSelection(authorState)).toBeNull();
    expect(validSelection({ ...state, showHidden: true })).toBe(imported.id);
  });

  it("shows clipLibrary registrations to everyone, not only to authors", () => {
    // A pack may ship nothing but a clipLibrary — the registrations ARE its content. They share
    // the osf.scene-clip/ id namespace with the harvested debug entries, so only `curated`
    // separates them; treating both as debug made such a pack invisible in the browser.
    // Both shapes as the engine actually serializes them: one anonymous role, unlisted, in the
    // library lane, tagged only `scene.clip` when the author supplied no tags of their own.
    const registered = normalizeScene({
      id: "osf.scene-clip/aaa",
      title: "Hand Extended 01",
      tags: ["scene.clip"],
      curated: true,
      unlisted: true,
      actorCount: 1,
      roles: [{ name: "", gender: "any" }],
      pack: "Moods of Andromas",
      folder: "Standing",
      stages: [{ index: 0, name: "Hand Extended 01", tags: ["scene.clip"], clipCount: 1, openEnded: true }],
    });
    const harvested = normalizeScene({
      id: "osf.scene-clip/bbb",
      title: "NAF\\RZSPU02.glb",
      tags: ["scene.clip"],
      unlisted: true,
      actorCount: 1,
      roles: [{ name: "", gender: "any" }],
      stages: [{ index: 0, name: "NAF\\RZSPU02.glb", tags: ["scene.clip"], clipCount: 1, openEnded: true }],
    });
    expect(isGeneratedSceneClip(registered)).toBe(false);
    expect(isGeneratedSceneClip(harvested)).toBe(true);

    // Stock preferences: author details off, poses-and-loops tier, custom+vanilla, no search.
    const state = { ...createInitialState(), library: [registered, harvested], libraryReceived: true };
    const visible = playableItems(state).filter((item) => playableVisible(state, item));
    expect(visible.map((item) => item.scene.id)).toEqual([registered.id]);
    expect(hiddenSceneCount(state)).toBe(1);
    // Ready with the default player-only cast, so it lands in the playable list, not the
    // "needs a different cast" bucket that stays folded away.
    expect(evaluateForState(state, registered).gaps).toBe(0);

    const authorState = { ...state, filters: { ...state.filters, debugMode: true } };
    expect(playableItems(authorState).filter((item) => playableVisible(authorState, item)).map((item) => item.scene.id))
      .toEqual([registered.id]);
    const revealedState = { ...state, showHidden: true };
    expect(playableItems(revealedState).filter((item) => playableVisible(revealedState, item)).map((item) => item.scene.id).sort())
      .toEqual([registered.id, harvested.id].sort());
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

});

describe("import report panel", () => {
  const file = (over: Partial<ImportFile>): ImportFile =>
    normalizeImportReport({ files: [over] }).files[0];

  it("keeps the two full-panel surfaces mutually exclusive", () => {
    const settings = browserReducer(createInitialState(), { type: "settings/open", open: true });
    const imports = browserReducer(settings, { type: "imports/open", open: true });
    expect(imports).toMatchObject({ importsOpen: true, settingsOpen: false });
    expect(browserReducer(imports, { type: "settings/open", open: true }))
      .toMatchObject({ importsOpen: false, settingsOpen: true });
  });

  it("closes with the rest of the console when the view is hidden", () => {
    const open = browserReducer(createInitialState(), { type: "imports/open", open: true });
    expect(browserReducer(open, { type: "visibility/hidden" }).importsOpen).toBe(false);
    expect(browserReducer(open, { type: "browser/opened", mode: "scenes", resetBrowsing: false }).importsOpen).toBe(false);
    expect(browserReducer(open, { type: "mode/changed", mode: "active" }).importsOpen).toBe(false);
  });

  it("tracks per-file disclosure independently", () => {
    const base = createInitialState();
    const one = browserReducer(base, { type: "imports/expanded", path: "a.osf.json", open: true });
    const two = browserReducer(one, { type: "imports/expanded", path: "b.osf.json", open: true });
    expect([...two.importsExpanded]).toEqual(["a.osf.json", "b.osf.json"]);
    expect([...browserReducer(two, { type: "imports/expanded", path: "a.osf.json", open: false }).importsExpanded])
      .toEqual(["b.osf.json"]);
  });

  it("ranks a file by its worst outcome", () => {
    expect(importSeverity(file({ scenes: 3 }))).toBe("ok");
    // Loaded without complaint but contributed nothing — never an error, always worth seeing.
    expect(importSeverity(file({}))).toBe("note");
    expect(importSeverity(file({ scenes: 3, warnings: 1 }))).toBe("warn");
    // Hidden scenes and missing clips are warnings even when the load itself said nothing.
    expect(importSeverity(file({ scenes: 3, hidden: 2 }))).toBe("warn");
    expect(importSeverity(file({ scenes: 3, missingClips: 4 }))).toBe("warn");
    expect(importSeverity(file({ scenes: 3, warnings: 1, errors: 1 }))).toBe("error");
  });

  it("sorts worst-first and filters by outcome and search", () => {
    const report = normalizeImportReport({ files: [
      { path: "a/clean.osf.json", file: "clean.osf.json", scenes: 2 },
      { path: "b/broken.osf.json", file: "broken.osf.json", errors: 1, problems: ["[error] 'broken.osf.json': schema 0 unsupported"] },
      { path: "c/warned.osf.json", file: "warned.osf.json", scenes: 1, warnings: 1, problems: ["[warn] 'warned.osf.json': 1 scene(s) hidden"] },
    ] });
    const state = { ...createInitialState(), imports: report.files, importsReceived: true, importsFilter: "all" as const };

    expect(visibleImports(state).map((entry) => entry.file))
      .toEqual(["broken.osf.json", "warned.osf.json", "clean.osf.json"]);
    expect(visibleImports({ ...state, importsFilter: "attention" }).map((entry) => entry.file))
      .toEqual(["broken.osf.json", "warned.osf.json"]);
    expect(visibleImports({ ...state, importsSearch: "schema 0" }).map((entry) => entry.file))
      .toEqual(["broken.osf.json"]);
    expect(visibleImports({ ...state, importsSearch: "a/clean" }).map((entry) => entry.file))
      .toEqual(["clean.osf.json"]);
  });

  it("explains outcomes and groups files for author triage", () => {
    const partial = file({ path: "Pack/scenes.osf.json", file: "scenes.osf.json", pack: "Pack A",
      declaredScenes: 2, scenes: 1, rejectedScenes: 1, errors: 1 });
    const rejected = file({ path: "Pack/bad.osf.json", file: "bad.osf.json", declaredScenes: 1,
      rejectedScenes: 1, rejected: true, errors: 1 });
    const missing = file({ path: "Pack/assets.osf.json", file: "assets.osf.json", scenes: 2, hidden: 2, missingClips: 3 });
    const empty = file({ path: "Pack/empty.osf.json", file: "empty.osf.json" });
    const clean = file({ path: "Other/clean.osf.json", file: "clean.osf.json", scenes: 2 });
	const routePartial = file({ path: "Suit/routes.osf.json", file: "routes.osf.json", declaredRoutes: 3, routes: 2, rejectedRoutes: 1, errors: 1 });

    expect([partial, rejected, missing, empty, clean].map(importOutcome))
      .toEqual(["partial", "rejected", "missing", "empty", "clean"]);
    expect(importResult(partial)).toBe("1 of 2 scenes loaded; 1 rejected.");
	expect(importResult(routePartial)).toBe("2 of 3 routes loaded; 1 rejected.");
    expect(importOutcomeCounts([partial, rejected, missing, empty, clean]))
      .toMatchObject({ all: 5, attention: 4, clean: 1, partial: 1, rejected: 1, missing: 1, empty: 1 });
    const grouped = importGroups({ ...createInitialState(), imports: [partial, rejected, missing, empty, clean], importsFilter: "all" });
    expect(grouped.map((group) => group.label)).toEqual(["Pack folder", "Pack A", "Other folder"]);
  });

  it("records reload deltas and jumps from a file to its loaded content", () => {
    const before = normalizeImportReport({ files: [{ path: "Pack/scenes.osf.json", file: "scenes.osf.json",
      errors: 1, problems: [{ code: "scene-invalid", message: "Bad role." }] }] });
    let state = { ...createInitialState(), imports: before.files, importTotals: before.totals, importsOpen: true };
    state = browserReducer(state, { type: "imports/reloadStarted" });
    expect(state.importReload.status).toBe("running");

    const after = normalizeImportReport({ files: [{ path: "Pack/scenes.osf.json", file: "scenes.osf.json", scenes: 1 }] });
    state = browserReducer(state, { type: "imports/reloadSucceeded", files: after.files, totals: after.totals,
      durationMs: 42, scenes: 1, completedAt: 100 });
    expect(state.importReload).toMatchObject({ status: "success", durationMs: 42, scenes: 1 });
    expect(state.importReload.delta.resolvedProblems).toHaveLength(1);

    const viewed = browserReducer(state, { type: "imports/viewContent", path: "Pack/scenes.osf.json" });
    expect(viewed).toMatchObject({ importsOpen: false, mode: "scenes", browseAll: true, showHidden: true, allSpecies: true });
    expect(viewed.filters).toEqual({ search: "pack/scenes.osf.json", debugMode: true });
  });

  it("formats sizes and durations at readable magnitudes", () => {
    // Under 10 KB keeps a decimal (small files differ meaningfully); above it rounds.
    expect([formatBytes(0), formatBytes(812), formatBytes(4_096), formatBytes(24_118), formatBytes(1_204_886)])
      .toEqual(["0 B", "812 B", "4.0 KB", "24 KB", "1.1 MB"]);
    expect([formatMillis(0), formatMillis(0.42), formatMillis(148.2), formatMillis(1_810)])
      .toEqual(["0 ms", "0.4 ms", "148 ms", "1.81 s"]);
  });
});
