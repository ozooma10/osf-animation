import { useCallback, useEffect, useMemo, useReducer, useRef } from "preact/hooks";
import type { BrowserCommands } from "./commands";
import { browserReducer } from "./reducer";
import {
  WHEEL_MAX,
  activeScenes,
  comparePlayableItems,
  hottestPickTarget,
  labeledCast,
  labeledFurniture,
  playableItems,
  playableSceneTitle,
  playableVisible,
  sceneById,
  sceneTitle,
  stageLabel,
  validSelection,
  wheelCandidates,
  wheelKey,
  wheelPool,
} from "./selectors";
import {
  PREFERENCE_KEYS,
  decodePreferences,
  preferenceFromChange,
  preferredOpenMode,
} from "./settings";
import {
  PLAYER_CAST,
  PLAYER_TOKEN,
  createInitialState,
  type ActiveScene,
  type ActorIndicator,
  type BrowserState,
  type BrowserPreferences,
  type CastMember,
  type NearbyTarget,
  type PickTarget,
  type WheelEntry,
} from "./state";
import { OsfUiBridge, hasOsfUiBridge, type AnimationBridge } from "../bridge/client";
import { isRecord, type BridgeCommand, type NativeMessage } from "../bridge/contract";
import { normalizeCatalog, type SceneModel } from "../model";
import type { DevCommands } from "../dev/debug";
import {
  NO_DEV_COMMANDS,
  createDebugCommands,
  createStandaloneBridge,
  installWheelHook,
  standaloneNearby,
} from "../dev/standalone";

// The `import.meta.env.DEV` guards below are written inline rather than through a
// local alias on purpose: Vite substitutes the literal `false` into a production
// build, which folds each branch away and lets the whole src/dev graph leave the
// bundle. Behind an alias the bundler keeps the references alive.

function normalizeNearby(payload: unknown): NearbyTarget[] {
  if (!Array.isArray(payload)) return [];
  return payload.filter(isRecord).map((item) => ({
    token: Number(item.token) || 0,
    name: String(item.name || "(unnamed)"),
    formId: Number(item.formId) || 0,
    distance: typeof item.distance === "number" ? item.distance : null,
    isActor: !!item.isActor,
    species: String(item.species || "").toLowerCase(),
    sex: String(item.sex || "").toLowerCase(),
    sceneCount: typeof item.sceneCount === "number" ? item.sceneCount : null,
    customCount: typeof item.customCount === "number" ? item.customCount : null,
    marker: !!item.marker,
  })).filter((item) => item.token !== 0);
}

function normalizeActive(payload: unknown): ActiveScene[] {
  if (!Array.isArray(payload)) return [];
  return payload.filter(isRecord).map((scene) => ({
    handle: Number(scene.handle) || 0,
    sceneId: String(scene.sceneId || ""),
    stage: Number.isInteger(scene.stage) ? scene.stage : 0,
    player: !!scene.player,
    cast: Array.isArray(scene.cast) ? scene.cast.filter(isRecord).map((member) => ({
      token: Number(member.token), name: String(member.name || "actor"), player: !!member.player,
    })) : [],
  })).filter((scene) => scene.handle > 0);
}

function normalizeIndicators(payload: unknown): ActorIndicator[] {
  if (!Array.isArray(payload)) return [];
  return payload.filter(isRecord).map((item) => ({
    token: Number(item.token) || 0,
    x: Number(item.x),
    y: Number(item.y),
    visible: !!item.visible,
  })).filter((item) => item.token !== 0 && Number.isFinite(item.x) && Number.isFinite(item.y));
}

function normalizePickTargets(payload: unknown): PickTarget[] {
  if (!Array.isArray(payload)) return [];
  return payload.filter(isRecord).map((item) => ({
    token: Number(item.token) || 0,
    x: Number(item.x),
    y: Number(item.y),
    cx: Number(item.cx),
    cy: Number(item.cy),
    rx: Number(item.rx),
    ry: Number(item.ry),
    // Tolerate a depth-less item (an older native side): 0 = treat as nearest,
    // which degrades front-most resolution to pure ellipse score — never drop a
    // target over a missing diagnostic-grade field.
    depth: Number.isFinite(Number(item.depth)) ? Number(item.depth) : 0,
  })).filter((item) => item.token !== 0 && [item.x, item.y, item.cx, item.cy].every(Number.isFinite)
    && item.rx > 0 && item.ry > 0);
}

function cloneWithPins(state: BrowserState, keys: readonly string[]): { catalog: SceneModel[]; library: SceneModel[] } {
  const positions = new Map(keys.map((key, index) => [key, index + 1]));
  const catalog = state.catalog.map((scene) => ({ ...scene, pinned: positions.get(wheelKey(scene.id, null)) ?? 0 }));
  const library = state.library.map((scene) => ({
    ...scene,
    pinned: 0,
    stages: scene.stages.map((stage) => ({ ...stage, pinned: positions.get(wheelKey(scene.id, stage.index)) ?? 0 })),
  }));
  return { catalog, library };
}

export function useBrowserController(): { state: BrowserState; commands: BrowserCommands; debugCommands: DevCommands; standalone: boolean } {
  const [state, dispatch] = useReducer(browserReducer, undefined, createInitialState);
  const stateRef = useRef(state);
  stateRef.current = state;
  // In game the host bridge is always present; simulating one there would hide a
  // broken bridge behind plausible fake data, so the fallback is dev-only.
  const standalone = useMemo(() => import.meta.env.DEV && !hasOsfUiBridge(), []);
  const bridge = useMemo<AnimationBridge>(() => import.meta.env.DEV && standalone
    ? createStandaloneBridge(() => stateRef.current)
    : new OsfUiBridge(), [standalone]);
  const handlerRef = useRef<(message: NativeMessage) => void>(() => undefined);
  const noticeTimer = useRef<number | undefined>();
  const catalogTimer = useRef<number | undefined>();
  const libraryTimer = useRef<number | undefined>();
  const catalogTries = useRef(0);
  const libraryTries = useRef(0);
  const lastAdvance = useRef(0);
  const padHeld = useRef<{ id: number; timer?: number }>({ id: 0 });

  const showNotice = useCallback((kind: "info" | "ok" | "err", text: string) => {
    dispatch({ type: "notice/show", kind, text });
    if (noticeTimer.current) clearTimeout(noticeTimer.current);
    if (kind !== "err") {
      const serial = stateRef.current.notice.serial + 1;
      noticeTimer.current = window.setTimeout(() => dispatch({ type: "notice/clear", serial }), 6000);
    }
  }, []);

  const emit = useCallback((message: NativeMessage) => handlerRef.current(message), []);

  const send = useCallback((command: BridgeCommand, fields: Record<string, unknown> = {}) => {
    bridge.send(command, fields);
  }, [bridge]);

  const requestCatalog = useCallback((fresh = false) => {
    if (fresh) { catalogTries.current = 0; dispatch({ type: "catalog/requested" }); }
    if (catalogTimer.current) clearTimeout(catalogTimer.current);
    send("osf.animation.catalog.get");
    if (standalone) return;
    if (catalogTries.current++ < 20) catalogTimer.current = window.setTimeout(() => requestCatalog(false), 1200);
    else if (!stateRef.current.catalogReceived) showNotice("err", "No response from OSF Animation. Load a save and make sure OSF UI is present.");
  }, [send, standalone, showNotice]);

  const requestLibrary = useCallback((fresh = false) => {
    if (fresh) { libraryTries.current = 0; dispatch({ type: "library/requested" }); }
    if (libraryTimer.current) clearTimeout(libraryTimer.current);
    send("osf.animation.library.get");
    if (standalone) return;
    if (libraryTries.current++ < 5) libraryTimer.current = window.setTimeout(() => requestLibrary(false), 1500);
    else if (!stateRef.current.libraryReceived) showNotice("err", "No response from OSF Animation. The animation library didn't load — make sure a save is loaded.");
  }, [send, standalone, showNotice]);

  handlerRef.current = (message) => {
    const payload = message.payload;
    const record = isRecord(payload) ? payload : {};
    switch (message.type) {
      case "runtime.ready":
        // Contract: a bridge being present (runtime.ready arriving) is the only gate — never
        // require a specific version field. The host sends `protocol` (see bridge.test.ts);
        // there is no `bridgeVersion` field, so gating on it wedged the view at "Engine Offline".
        dispatch({ type: "runtime/ready" });
        showNotice("ok", `Bridge online. Protocol ${record.protocol || "?"}.`);
        send("osfui.gamepadRaw", { raw: true });
        requestCatalog(true);
        requestLibrary(true);
        break;
      case "osf.animation.version": dispatch({ type: "plugin/received", plugin: record }); break;
      case "settings.data": {
        const mods = Array.isArray(record.mods) ? record.mods : [];
        const animation = mods.find((mod) => isRecord(mod) && mod.id === "osf.animation");
        const values = isRecord(animation) && isRecord(animation.values) ? animation.values : {};
        const preferences = decodePreferences(values);
        dispatch({ type: "settings/received", preferences });
        // settings.data can race the host's first visibility message. Apply the
        // opening preference here as well so a newly mounted view does not
        // briefly settle on the hard-coded Scenes default.
        const current = stateRef.current;
        const merged = { ...current.preferences, ...preferences };
        if (!current.wheel) {
          const mode = preferredOpenMode(merged.openTo, current.lastBrowseMode, activeScenes(current).length > 0);
          dispatch({ type: "browser/opened", mode, resetBrowsing: !merged.rememberBrowsing });
          if (!current.libraryReceived) requestLibrary(true);
        }
        break;
      }
      case "settings.changed":
        if (record.mod === "osf.animation") dispatch({ type: "settings/received", preferences: preferenceFromChange(record.key, record.value) });
        break;
      case "osf.animation.catalog.data":
        if (catalogTimer.current) clearTimeout(catalogTimer.current);
        dispatch({ type: "catalog/received", scenes: normalizeCatalog(payload) });
        break;
      case "osf.animation.library.data":
        if (libraryTimer.current) clearTimeout(libraryTimer.current);
        dispatch({ type: "library/received", scenes: normalizeCatalog(payload, true) });
        break;
      case "osf.animation.wheel.data": {
        if (!stateRef.current.wheel) break;
        const entries: WheelEntry[] = Array.isArray(record.entries) ? record.entries.filter(isRecord).map((entry) => {
          const scene = String(entry.scene || "");
          const stage = Number.isInteger(entry.stage) ? entry.stage : null;
          const title = String(entry.title || scene || "Animation");
          return { scene, stage, title, detail: String(entry.detail || title), key: wheelKey(scene, stage) };
        }).filter((entry) => entry.scene) : [];
        dispatch({ type: "wheel/received", customized: !!record.customized, entries });
        break;
      }
      case "osf.animation.pick": {
        // A miss keeps the pick armed — re-arming per attempt made the feature
        // read as broken. Actor picks also stay armed on success (crew building
        // is a multi-click flow; Esc or CANCEL finishes); a furniture hit
        // disarms because there is exactly one anchor slot to fill.
        // A local miss never reaches this handler (pickAt resolves clicks against the
        // marker geometry) — an invalid reply means the clicked target vanished or became
        // ineligible between the marker poll and the click.
        if (!record.valid || !record.token) { showNotice("err", `That ${record.slot === "furniture" ? "furniture" : "actor"} is no longer available — pick again, or use SCAN.`); break; }
        if (record.slot === "furniture") {
          dispatch({ type: "pick/cancelled" });
          dispatch({ type: "anchor/selected", anchor: { token: Number(record.token), name: String(record.name || "furniture"), distance: typeof record.distance === "number" ? record.distance : null } });
          send("osf.animation.anchorMatch", { token: Number(record.token) });
        } else {
          const member: CastMember = Number(record.token) === PLAYER_TOKEN ? PLAYER_CAST : { token: Number(record.token), name: String(record.name || "actor"), distance: typeof record.distance === "number" ? record.distance : null, species: String(record.species || "human"), sex: String(record.sex || "").toLowerCase() };
          const removing = stateRef.current.cast.some((candidate) => candidate.token === member.token);
          dispatch({ type: "cast/toggled", member });
          showNotice("ok", `${member.name} ${removing ? "removed from" : "added to"} the crew — click more, Esc to finish.`);
        }
        break;
      }
      case "osf.animation.openTarget": {
        const token = Number(record.token);
        if (!token || stateRef.current.seededTokens.has(token)) break;
        dispatch({ type: "seeded/remembered", token });
        if (record.slot === "furniture" && !stateRef.current.furniture) {
          dispatch({ type: "anchor/selected", anchor: { token, name: String(record.name || "furniture"), distance: typeof record.distance === "number" ? record.distance : null } });
          send("osf.animation.anchorMatch", { token });
        } else if (record.slot !== "furniture") {
          const member: CastMember = { token, name: String(record.name || "actor"), distance: typeof record.distance === "number" ? record.distance : null, species: String(record.species || "human"), sex: String(record.sex || "").toLowerCase() };
          dispatch(stateRef.current.cast.length === 1 && stateRef.current.cast[0].kind === "player" ? { type: "cast/replaced", members: [member] } : { type: "cast/toggled", member });
        }
        break;
      }
      case "osf.animation.scanResults": {
        const kind = record.kind === "furniture" ? "furniture" : "actor";
        const targets = normalizeNearby(record.items);
        dispatch({ type: "nearby/received", kind, targets });
        showNotice("info", `${targets.length} nearby ${kind === "actor" ? `actor${targets.length === 1 ? "" : "s"}` : `furniture spot${targets.length === 1 ? "" : "s"}`} found.`);
        break;
      }
      case "osf.animation.actorIndicators":
        dispatch({ type: "indicators/received", items: normalizeIndicators(record.items) });
        break;
      case "osf.animation.pickTargets":
        dispatch({ type: "pickTargets/received", slot: record.slot === "furniture" ? "furniture" : "actor", items: normalizePickTargets(record.items) });
        break;
      case "osf.animation.anchorMatch": dispatch({ type: "anchor/matched", token: Number(record.token), ids: new Set(Array.isArray(record.sceneIds) ? record.sceneIds.map(String) : []) }); break;
      case "osf.animation.activeScenes": dispatch({ type: "active/received", scenes: normalizeActive(record.scenes) }); break;
      case "osf.animation.launchResult":
        if (record.ok && record.handle) {
          const sceneId = String(record.sceneId || stateRef.current.selectedId || "");
          const wheelLaunch = !!stateRef.current.wheel;
          const afterLaunch = stateRef.current.preferences.afterLaunch;
          dispatch({ type: "launch/succeeded", handle: Number(record.handle), sceneId, afterLaunch });
          if (wheelLaunch || afterLaunch === "close") send("osf.animation.requestClose");
          else showNotice("ok", `Playing "${sceneTitle(stateRef.current, sceneId)}" on handle ${record.handle}.`);
        } else {
          const error = String(record.error || "Launch failed.");
          dispatch({ type: "launch/failed", error });
          if (!stateRef.current.wheel) showNotice("err", error);
        }
        break;
      case "osf.animation.notice": if (record.text) showNotice(record.kind === "err" || record.kind === "ok" ? record.kind : "info", String(record.text)); break;
      case "osf.animation.mode": {
        if (record.mode !== "wheel") { dispatch({ type: "wheel/exited" }); break; }
        const target = isRecord(record.target) && typeof record.target.token === "number" ? { token: record.target.token, name: String(record.target.name || "Target") } : null;
        const tagPrefix = String(record.tagPrefix || "player.emote.").toLowerCase();
        dispatch({ type: "wheel/entered", tagPrefix, target });
        window.setTimeout(() => { if (!stateRef.current.wheel?.requested) { dispatch({ type: "wheel/requested" }); send("osf.animation.wheel.get", { tagPrefix }); } }, 0);
        break;
      }
      case "ui.visibility":
        if (!record.visible) {
          if (padHeld.current.timer) clearTimeout(padHeld.current.timer);
          padHeld.current = { id: 0 };
          dispatch({ type: "visibility/hidden" });
        } else {
          dispatch({ type: "visibility/shown" });
          const current = stateRef.current;
          const mode = preferredOpenMode(current.preferences.openTo, current.lastBrowseMode, activeScenes(current).length > 0);
          dispatch({ type: "browser/opened", mode, resetBrowsing: !current.preferences.rememberBrowsing });
          if (!current.libraryReceived) requestLibrary(true);
        }
        send(record.visible ? "osf.animation.opened" : "osf.animation.closed");
        break;
      case "ui.error": showNotice("err", `Bridge rejected a message: ${record.message || record.code || "unknown error"}`); break;
      case "ui.gamepad": handleGamepad(record); break;
    }
  };

  const handleGamepad = (payload: Record<string, any>) => {
    if (payload.kind !== "button" || !isRecord(payload.button)) return;
    const id = Number(payload.button.id) || 0;
    const stopRepeat = () => { if (padHeld.current.timer) clearTimeout(padHeld.current.timer); padHeld.current = { id: 0 }; };
    if (!payload.button.down) { if (id === padHeld.current.id) stopRepeat(); return; }
    const key = ({ 0x0001: "ArrowUp", 0x0002: "ArrowDown", 0x0004: "ArrowLeft", 0x0008: "ArrowRight", 0x1000: "Enter", 0x2000: "Escape" } as Record<number, string>)[id];
    if (!key) return;
    const tap = () => document.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }));
    stopRepeat();
    tap();
    if (id <= 0x0008) {
      padHeld.current.id = id;
      const repeat = () => { tap(); padHeld.current.timer = window.setTimeout(repeat, 110); };
      padHeld.current.timer = window.setTimeout(repeat, 350);
    }
  };

  useEffect(() => {
    const unsubscribe = bridge.subscribe((message) => emit(message));
    if (import.meta.env.DEV && standalone) {
      dispatch({ type: "runtime/ready" });
      const nearby = standaloneNearby();
      dispatch({ type: "nearby/received", kind: "actor", targets: normalizeNearby(nearby.actors) });
      dispatch({ type: "nearby/received", kind: "furniture", targets: normalizeNearby(nearby.anchors) });
      requestCatalog(true);
      requestLibrary(true);
      showNotice("info", "Standalone mode. Snapshot catalog; pick/scan/launch are stubbed. W = animation wheel · B = backdrop.");
      installWheelHook(bridge);
    } else {
      requestCatalog(true);
      requestLibrary(true);
    }
    send("settings.get");
    return () => { unsubscribe(); bridge.dispose(); if (catalogTimer.current) clearTimeout(catalogTimer.current); if (libraryTimer.current) clearTimeout(libraryTimer.current); if (noticeTimer.current) clearTimeout(noticeTimer.current); if (padHeld.current.timer) clearTimeout(padHeld.current.timer); };
  }, []);

  useEffect(() => {
    if (state.mode === "active") {
      const selectedId = validSelection(state);
      if (selectedId !== state.selectedId) dispatch({ type: "selection/changed", sceneId: selectedId });
      return;
    }
    // Catalog and library replies are independent. Do not let whichever lane
    // arrives first become an accidental sticky default.
    if (!state.selectedId && (!state.catalogReceived || !state.libraryReceived)) return;
    const visible = playableItems(state)
      .filter((item) => playableVisible(state, item))
      .sort((a, b) => comparePlayableItems(state, a, b));
    const currentValid = visible.some((item) => item.scene.id === state.selectedId
      && (item.stage?.index ?? null) === state.selectedStage);
    if (!currentValid) {
      const first = visible[0];
      dispatch({ type: "selection/changed", sceneId: first?.scene.id ?? null, stage: first?.stage?.index ?? null });
    }
  }, [state.catalog, state.catalogReceived, state.library, state.libraryReceived, state.mode, state.filters, state.allSpecies, state.browseAll, state.browseKind, state.libCustomOnly, state.libFull, state.libShowAll, state.cast, state.furniture, state.anchorMatch]);

  useEffect(() => {
    document.body.classList.toggle("wheel-mode", !!state.wheel);
    document.body.classList.toggle("live-mode", state.minimized);
  }, [state.wheel, state.minimized]);

  useEffect(() => {
    // Nothing renders the projections when the label layer is dark, so skip the
    // 80ms round-trip too. Cast and selected furniture share the label policy.
    const tokens = labeledCast(state).map(({ member }) => member.token);
    const furniture = labeledFurniture(state);
    if (furniture) tokens.push(furniture.token);
    if (tokens.length === 0) {
      if (state.actorIndicators.length) dispatch({ type: "indicators/received", items: [] });
      return;
    }
    const project = () => send("osf.animation.projectActors", { tokens, width: innerWidth, height: innerHeight });
    project();
    const timer = window.setInterval(project, 80);
    return () => clearInterval(timer);
  }, [state.cast, state.furniture, state.viewVisible, state.wheel, state.minimized, state.preferences.actorLabels, send]);

  useEffect(() => {
    // Armed pick: poll the pickable targets' screen geometry so the view can
    // mark them — hover-only for actors, all-targets for furniture. Same
    // cadence family as the cast label projections; the native side reuses
    // the click hit-test geometry, so marker shown === click would land.
    const slot = state.pickMode;
    if (!slot || !state.viewVisible) return;
    const project = () => send("osf.animation.projectPickables", { slot, width: innerWidth, height: innerHeight });
    project();
    const timer = window.setInterval(project, 100);
    return () => clearInterval(timer);
  }, [state.pickMode, state.viewVisible, send]);

  const commands = useMemo<BrowserCommands>(() => ({
    refresh: () => { requestCatalog(true); requestLibrary(true); },
    setMode: (mode) => { dispatch({ type: "mode/changed", mode: mode === "library" ? "scenes" : mode }); if (!stateRef.current.libraryReceived) requestLibrary(true); },
    selectScene: (sceneId, stage = null) => dispatch({ type: "selection/changed", sceneId, stage }),
    setSearch: (search) => dispatch({ type: "filter/search", search: search.trim().toLowerCase() }),
    toggleSettings: (open) => dispatch({ type: "settings/open", open: open ?? !stateRef.current.settingsOpen }),
    setPreference: (key, value) => {
      dispatch({ type: "settings/received", preferences: { [key]: value } as Partial<BrowserPreferences> });
      send("settings.set", { mod: "osf.animation", key: PREFERENCE_KEYS[key], value });
    },
    toggleBrowseAll: () => dispatch({ type: "browse/all" }),
    setBrowseKind: (kind) => dispatch({ type: "browse/kind", kind }),
    toggleSpecies: () => dispatch({ type: "filter/species" }),
    toggleStep: (step) => dispatch({ type: "step/toggled", step }),
    toggleMarkers: () => dispatch({ type: "markers/toggled" }),
    scan: (kind) => { showNotice("info", `Scanning nearby ${kind === "furniture" ? "furniture" : "actors"}…`); send("osf.animation.scanNearby", { kind, sceneId: stateRef.current.selectedId || "" }); },
    pick: (kind) => {
      const armed = stateRef.current.pickMode === kind;
      dispatch(armed ? { type: "pick/cancelled" } : { type: "pick/armed", kind });
      showNotice("info", armed ? "World selection cancelled." : `Click the ${kind === "actor" ? "actor" : "furniture"} in the visible world. Drag still orbits; Esc cancels.`);
    },
    pickAt: (x, y, width, height) => {
      const slot = stateRef.current.pickMode;
      if (!slot) return;
      // Resolve the click against the SAME marker geometry the user is looking at
      // (hottestPickTarget over the polled pick targets). No native re-projection at
      // click time: the hot marker's token IS the selection, and a miss is decided
      // right here — so the marker shown and the click result can never disagree.
      const hot = hottestPickTarget(stateRef.current.pickTargets, x * width, y * height, width, height);
      if (!hot) {
        showNotice("err", `No ${slot === "furniture" ? "furniture" : "actor"} was under that click — aim for its marker, or use SCAN.`);
        return;
      }
      send("osf.animation.pickScreen", { slot, token: hot.token });
    },
    cancelPick: () => { if (stateRef.current.pickMode) { dispatch({ type: "pick/cancelled" }); showNotice("info", "World selection cancelled."); } },
    toggleActor: (token) => { const actor = stateRef.current.nearbyActors.find((candidate) => candidate.token === token); if (actor) dispatch({ type: "cast/toggled", member: { token, name: actor.name, distance: actor.distance, species: actor.species || "human", sex: actor.sex } }); },
    togglePlayer: () => dispatch({ type: "cast/toggled", member: PLAYER_CAST }),
    removeMember: (index) => dispatch({ type: "cast/removed", index }),
    moveMember: (index, delta) => dispatch({ type: "cast/moved", from: index, to: index + delta, after: delta > 0 }),
    reorderMember: (from, to, after) => dispatch({ type: "cast/moved", from, to, after }),
    toggleAnchor: (token) => { const current = stateRef.current; if (current.furniture?.token === token) dispatch({ type: "anchor/cleared" }); else { const anchor = current.nearbyFurniture.find((candidate) => candidate.token === token); if (anchor) { dispatch({ type: "anchor/selected", anchor: { token, name: anchor.name, distance: anchor.distance } }); send("osf.animation.anchorMatch", { token }); } } },
    clearAnchor: () => dispatch({ type: "anchor/cleared" }),
    selectLocation: (mode, token = null) => dispatch({ type: "location/selected", mode, token }),
    toggleLibraryGroup: (key, open) => dispatch({ type: "library/group", key, open }),
    toggleSceneGroup: (key, open) => dispatch({ type: "scene/group", key, open }),
    toggleLibraryShowAll: () => dispatch({ type: "library/showAll" }),
    toggleLibraryFull: () => {
      const value = stateRef.current.preferences.libraryDetail === "full" ? "curated" : "full";
      dispatch({ type: "settings/received", preferences: { libraryDetail: value } });
      send("settings.set", { mod: "osf.animation", key: PREFERENCE_KEYS.libraryDetail, value });
    },
    toggleLibraryCustomOnly: () => {
      const value = stateRef.current.preferences.librarySource === "custom" ? "all" : "custom";
      dispatch({ type: "settings/received", preferences: { librarySource: value } });
      send("settings.set", { mod: "osf.animation", key: PREFERENCE_KEYS.librarySource, value });
    },
    toggleBriefAnimations: () => dispatch({ type: "brief/fullAnimations" }),
    toggleOptions: () => dispatch({ type: "brief/options" }),
    setOption: (field, value) => dispatch({ type: "brief/option", field, value }),
    launch: (stageIndex, singleAnimation = false, sceneId) => {
      const current = stateRef.current;
      const scene = sceneById(current, sceneId ?? current.selectedId);
      if (!scene) return;
      const effectiveStage = Number.isInteger(stageIndex) ? Number(stageIndex) : sceneId ? null : current.selectedStage;
      const stageOnly = singleAnimation || (!!scene.library && effectiveStage != null);
      const options: Record<string, unknown> = { strip: Number(current.opts.strip), lockPlayer: Number(current.opts.lock), camera: current.opts.camera, speed: Number(current.opts.speed) };
      if (effectiveStage != null && effectiveStage > 0) options.stage = effectiveStage;
      const fields: Record<string, unknown> = {
        sceneId: scene.id,
        castTokens: current.cast.map((member) => member.token),
        opts: options,
        singleAnimation: stageOnly,
      };
      fields.location = {
        mode: scene.requiresFurniture ? "furniture" : scene.inPlace ? "cast" : current.locationMode,
        token: scene.requiresFurniture ? current.furniture?.token ?? 0 : current.locationToken ?? 0,
      };
      const roleNames = scene.roles.map((role) => role.name);
      if (roleNames.length === current.cast.length && roleNames.every((name) => name && !/^role \d+$/i.test(name))) fields.roleNames = roleNames;
      if (scene.requiresFurniture && current.furniture) fields.furnitureToken = current.furniture.token;
      const title = playableSceneTitle(scene);
      showNotice("info", `Launching "${effectiveStage != null ? `${title} · ${stageLabel(scene, effectiveStage)}` : title}"…`);
      send("osf.animation.launch", fields);
    },
    stop: (handle) => { const target = Number(handle) || stateRef.current.lastHandle; if (!target) return; send("osf.animation.stop", { handle: target }); dispatch({ type: "scene/stopped", handle: target }); showNotice("info", `Stopping handle ${target}…`); },
    stopAll: () => { for (const scene of activeScenes(stateRef.current)) { send("osf.animation.stop", { handle: scene.handle }); dispatch({ type: "scene/stopped", handle: scene.handle }); } },
    advance: (handle) => { const target = Number(handle) || stateRef.current.lastHandle || (activeScenes(stateRef.current).length === 1 ? activeScenes(stateRef.current)[0].handle : 0); if (!target || Date.now() - lastAdvance.current < 350) return; lastAdvance.current = Date.now(); send("osf.animation.advance", { handle: target }); },
    setMinimized: (minimized) => dispatch({ type: "minimized/changed", minimized }),
    toggleWheelEntry: (scene, stage = null) => {
      const current = stateRef.current;
      const key = wheelKey(scene, stage);
      const candidate = wheelCandidates(current).find((item) => item.key === key);
      if (!candidate) return;
      const entries = wheelPool(current).slice(0, WHEEL_MAX);
      const index = entries.findIndex((item) => item.key === key);
      if (index < 0) { if (entries.length >= WHEEL_MAX) { showNotice("err", `The animation wheel is full (${WHEEL_MAX}/${WHEEL_MAX}).`); return; } entries.push(candidate); } else entries.splice(index, 1);
      const pinned = cloneWithPins(current, entries.map((entry) => entry.key));
      dispatch({ type: "wheel/customized", ...pinned });
      send("osf.animation.wheel.set", { entries: entries.map((entry) => entry.stage == null ? { scene: entry.scene } : { scene: entry.scene, stage: entry.stage }) });
    },
    moveWheelEntry: (scene, stage, direction) => {
      const current = stateRef.current;
      const entries = wheelPool(current).slice(0, WHEEL_MAX);
      const from = entries.findIndex((entry) => entry.key === wheelKey(scene, stage));
      const to = from + direction;
      if (from < 0 || to < 0 || to >= entries.length) return;
      [entries[from], entries[to]] = [entries[to], entries[from]];
      dispatch({ type: "wheel/customized", ...cloneWithPins(current, entries.map((entry) => entry.key)) });
      send("osf.animation.wheel.set", { entries: entries.map((entry) => entry.stage == null ? { scene: entry.scene } : { scene: entry.scene, stage: entry.stage }) });
    },
    resetWheel: () => { const pinned = cloneWithPins(stateRef.current, []); dispatch({ type: "wheel/reset", ...pinned }); send("osf.animation.wheel.set", { reset: true }); showNotice("info", "Animation wheel reset to installed defaults."); },
    focusWheel: (index) => dispatch({ type: "wheel/focused", focus: index }),
    pickWheel: (index) => { const current = stateRef.current; const wheel = current.wheel; if (!wheel || wheel.launching) return; const entry = wheel.entries[index]; if (!entry) return; dispatch({ type: "wheel/launching", key: entry.key }); send("osf.animation.launch", { sceneId: entry.scene, castTokens: [wheel.target?.token ?? PLAYER_TOKEN], opts: entry.stage == null ? {} : { stage: entry.stage } }); },
    cancelWheel: () => send("osf.animation.requestClose"),
    requestClose: () => send("osf.animation.requestClose"),
    orbit: (dx, dy, wheel) => send("osf.animation.orbit", { dx, dy, wheel }),
    openModPage: (url) => { if (standalone) window.open(url, "_blank", "noopener"); else send("osfui.openModPage"); },
  }), [requestCatalog, requestLibrary, send, showNotice, standalone]);

  const debugCommands = useMemo<DevCommands>(
    () => import.meta.env.DEV ? createDebugCommands({ dispatch, send, requestCatalog, stateRef }) : NO_DEV_COMMANDS,
    [requestCatalog, send],
  );

  return { state, commands, debugCommands, standalone };
}
