import { useCallback, useEffect, useMemo, useReducer, useRef } from "preact/hooks";
import type { BrowserCommands } from "./commands";
import { browserReducer } from "./reducer";
import {
  activeLaunches,
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
  type ActiveLaunch,
  type ActorIndicator,
  type BrowserState,
  type BrowserPreferences,
  type CastMember,
  type NearbyTarget,
  type PickTarget,
} from "./state";
import { OsfUiBridge, hasOsfUiBridge, type AnimationBridge } from "../bridge/client";
import { isRecord, type BridgeCommand, type NativeMessage } from "../bridge/contract";
import { normalizeCatalog } from "../model";
import type { DevCommands } from "../dev/debug";
import {
  NO_DEV_COMMANDS,
  createDebugCommands,
  createStandaloneBridge,
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

export function normalizeActive(payload: unknown): ActiveLaunch[] {
  if (!Array.isArray(payload)) return [];
  return payload.filter(isRecord).map((scene): ActiveLaunch => ({
    handle: Number(scene.handle) || 0,
    sceneId: String(scene.sceneId || ""),
    stage: Number.isInteger(scene.stage) ? scene.stage : 0,
    player: !!scene.player,
    time: Math.max(0, Number(scene.time) || 0),
    duration: Math.max(0, Number(scene.duration) || 0),
    speed: Math.max(0, Number(scene.speed) || 0),
    cast: Array.isArray(scene.cast) ? scene.cast.filter(isRecord).map((member) => ({
      token: Number(member.token), name: String(member.name || "actor"), player: !!member.player,
    })) : [],
  })).filter((scene) => scene.handle !== 0);
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
        // The mount effect already requested both; re-request only what hasn't landed —
        // the native side rebuilds and serializes the ~MB library payload per library.get,
        // so the unconditional duplicate cost a full extra build every view creation.
        if (!stateRef.current.catalogReceived) requestCatalog(true);
        if (!stateRef.current.libraryReceived) requestLibrary(true);
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
        const mode = preferredOpenMode(merged.openTo, current.lastBrowseMode, activeLaunches(current).length > 0);
        dispatch({ type: "browser/opened", mode, resetBrowsing: !merged.rememberBrowsing });
        if (!current.libraryReceived) requestLibrary(true);
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
      case "osf.animation.pick": {
        // A miss keeps the pick armed — re-arming per attempt made the feature
        // read as broken. Actor picks also stay armed on success (cast building
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
          showNotice("ok", `${member.name} ${removing ? "removed from" : "added to"} the cast — click more, Esc to finish.`);
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
          const afterLaunch = stateRef.current.preferences.afterLaunch;
          dispatch({ type: "launch/succeeded", handle: Number(record.handle), sceneId, afterLaunch });
          if (afterLaunch === "close") send("osf.animation.requestClose");
          else showNotice("ok", `Playing "${sceneTitle(stateRef.current, sceneId)}" on handle ${record.handle}.`);
        } else {
          const error = String(record.error || "Launch failed.");
          dispatch({ type: "launch/failed", error });
          showNotice("err", error);
        }
        break;
      case "osf.animation.notice": if (record.text) showNotice(record.kind === "err" || record.kind === "ok" ? record.kind : "info", String(record.text)); break;
      case "ui.visibility":
        if (!record.visible) {
          if (padHeld.current.timer) clearTimeout(padHeld.current.timer);
          padHeld.current = { id: 0 };
          dispatch({ type: "visibility/hidden" });
        } else {
          dispatch({ type: "visibility/shown" });
          const current = stateRef.current;
          const mode = preferredOpenMode(current.preferences.openTo, current.lastBrowseMode, activeLaunches(current).length > 0);
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
      showNotice("info", "Standalone mode. Snapshot catalog; pick/scan/launch are stubbed. B = backdrop.");
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
  }, [state.catalog, state.catalogReceived, state.library, state.libraryReceived, state.mode, state.filters, state.allSpecies, state.browseAll, state.showHidden, state.browseKind, state.libCustomOnly, state.libFull, state.cast, state.furniture, state.anchorMatch]);

  useEffect(() => {
    document.body.classList.toggle("live-mode", state.minimized);
  }, [state.minimized]);

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
  }, [state.cast, state.furniture, state.viewVisible, state.minimized, state.preferences.actorLabels, send]);

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

  // Playback poll feeds the ACTIVE list's runtime clocks.
  useEffect(() => {
    if (state.mode !== "active" || !state.viewVisible || !state.active?.length) return;
    const refreshPlayback = () => send("osf.animation.playback.get");
    refreshPlayback();
    const timer = window.setInterval(refreshPlayback, 100);
    return () => clearInterval(timer);
  }, [state.mode, state.viewVisible, state.active?.length, send]);

  const startPlayable = useCallback((stageIndex?: number, singleAnimation = false, sceneId?: string) => {
    const current = stateRef.current;
    const scene = sceneById(current, sceneId ?? current.selectedId);
    if (!scene) return;
    const effectiveStage = Number.isInteger(stageIndex) ? Number(stageIndex) : sceneId ? null : current.selectedStage;
    const stageOnly = singleAnimation || (!!scene.library && effectiveStage != null);
    const options: Record<string, unknown> = {
      hideApparel: Number(current.opts.hideApparel),
      playerInputLock: Number(current.opts.playerInputLock),
      camera: current.opts.camera,
      speed: Number(current.opts.speed),
    };
    if (effectiveStage != null && effectiveStage > 0) options.stage = effectiveStage;
    const castTokens = current.cast.map((member) => member.token);
    const fields: Record<string, unknown> = {
      sceneId: scene.id,
      castTokens,
      opts: options,
      singleAnimation: stageOnly,
    };
    fields.location = {
      mode: scene.requiresFurniture ? "furniture" : scene.worldPlacement === "followActor" ? "cast" : current.locationMode,
      token: scene.requiresFurniture ? current.furniture?.token ?? 0 : current.locationToken ?? 0,
    };
    const roleNames = scene.roles.map((role) => role.name);
    if (roleNames.length === castTokens.length && roleNames.every((name) => name && !/^role \d+$/i.test(name))) fields.roleNames = roleNames;
    const furnitureToken = scene.requiresFurniture && current.furniture ? current.furniture.token : undefined;
    if (furnitureToken != null) fields.furnitureToken = furnitureToken;
    const title = playableSceneTitle(scene);
    showNotice("info", `Launching "${effectiveStage != null ? `${title} · ${stageLabel(scene, effectiveStage)}` : title}"…`);
    send("osf.animation.launch", fields);
  }, [send, showNotice]);

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
    toggleHidden: () => dispatch({ type: "browse/hidden" }),
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
    launch: startPlayable,
    stop: (handle) => { const target = Number(handle) || stateRef.current.lastHandle; if (!target) return; send("osf.animation.stop", { handle: target }); dispatch({ type: "active/stopped", handle: target }); showNotice("info", `Stopping handle ${target}…`); },
    stopAll: () => { for (const launch of activeLaunches(stateRef.current)) { send("osf.animation.stop", { handle: launch.handle }); dispatch({ type: "active/stopped", handle: launch.handle }); } },
    advance: (handle) => {
      const current = stateRef.current;
      const launches = activeLaunches(current);
      const target = Number(handle) || current.lastHandle || (launches.length === 1 ? launches[0].handle : 0);
      if (!target || Date.now() - lastAdvance.current < 350) return;
      lastAdvance.current = Date.now();
      send("osf.animation.advance", { handle: target });
    },
    setPlayback: (handle, paused) => send("osf.animation.playback.set", { handle, paused }),
    setMinimized: (minimized) => dispatch({ type: "minimized/changed", minimized }),
    requestClose: () => send("osf.animation.requestClose"),
    orbit: (dx, dy, wheel) => send("osf.animation.orbit", { dx, dy, wheel }),
    openModPage: (url) => { if (standalone) window.open(url, "_blank", "noopener"); else send("osfui.openModPage"); },
  }), [requestCatalog, requestLibrary, send, showNotice, standalone, startPlayable]);

  const debugCommands = useMemo<DevCommands>(
    () => import.meta.env.DEV ? createDebugCommands({ dispatch }) : NO_DEV_COMMANDS,
    [],
  );

  return { state, commands, debugCommands, standalone };
}
