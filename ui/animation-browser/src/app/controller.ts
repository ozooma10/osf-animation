import { useCallback, useEffect, useMemo, useReducer, useRef } from "preact/hooks";
import type { BrowserCommands } from "./commands";
import { browserReducer } from "./reducer";
import {
  WHEEL_MAX,
  activeScenes,
  sceneById,
  sceneTitle,
  stageLabel,
  validSelection,
  wheelCandidates,
  wheelKey,
  wheelPool,
} from "./selectors";
import {
  PLAYER_CAST,
  PLAYER_TOKEN,
  createInitialState,
  type ActiveScene,
  type BrowserState,
  type CastMember,
  type NearbyTarget,
  type WheelEntry,
} from "./state";
import { OsfUiBridge, hasOsfUiBridge, type AnimationBridge } from "../bridge/client";
import { isRecord, type BridgeCommand, type NativeMessage } from "../bridge/contract";
import { safeNormalizeScene, type SceneModel } from "../model";
import { MOCK_ACTORS, MOCK_ANCHORS } from "../dev/mock-data";
import { StandaloneBridge } from "../dev/mock-bridge";
import { MOCK_CATALOG } from "../dev/mock-data";
import type { DevCommands, VersionDebugState, WheelDebugState } from "../dev/DevTools";

function normalizeCatalog(payload: unknown, library = false): SceneModel[] {
  if (!Array.isArray(payload)) return [];
  return payload.map(safeNormalizeScene).filter((scene): scene is SceneModel => !!scene).map((scene) => library
    ? { ...scene, library: true, stageHay: scene.stages.map((stage) => stage.name).join(" ").toLowerCase() }
    : scene);
}

function normalizeNearby(payload: unknown): NearbyTarget[] {
  if (!Array.isArray(payload)) return [];
  return payload.filter(isRecord).map((item) => ({
    token: Number(item.token) || 0,
    name: String(item.name || "(unnamed)"),
    formId: Number(item.formId) || 0,
    distance: typeof item.distance === "number" ? item.distance : null,
    isActor: !!item.isActor,
    species: String(item.species || "").toLowerCase(),
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
  const standalone = useMemo(() => !hasOsfUiBridge(), []);
  const bridge = useMemo<AnimationBridge>(() => standalone
    ? new StandaloneBridge(() => stateRef.current)
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
        break;
      case "osf.animation.version": dispatch({ type: "plugin/received", plugin: record }); break;
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
        if (!record.valid || !record.token) { showNotice("err", `No ${record.slot || "target"} was under the crosshair when the browser opened — aim first, then open, or use SCAN.`); break; }
        if (record.slot === "furniture") {
          dispatch({ type: "anchor/selected", anchor: { token: Number(record.token), name: String(record.name || "furniture"), distance: typeof record.distance === "number" ? record.distance : null } });
          send("osf.animation.anchorMatch", { token: Number(record.token) });
        } else {
          const member: CastMember = Number(record.token) === PLAYER_TOKEN ? PLAYER_CAST : { token: Number(record.token), name: String(record.name || "actor"), distance: typeof record.distance === "number" ? record.distance : null, species: String(record.species || "human") };
          dispatch({ type: "cast/toggled", member });
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
          const member: CastMember = { token, name: String(record.name || "actor"), distance: typeof record.distance === "number" ? record.distance : null, species: String(record.species || "human") };
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
      case "osf.animation.anchorMatch": dispatch({ type: "anchor/matched", token: Number(record.token), ids: new Set(Array.isArray(record.sceneIds) ? record.sceneIds.map(String) : []) }); break;
      case "osf.animation.activeScenes": dispatch({ type: "active/received", scenes: normalizeActive(record.scenes) }); break;
      case "osf.animation.launchResult":
        if (record.ok && record.handle) {
          const sceneId = String(record.sceneId || stateRef.current.selectedId || "");
          dispatch({ type: "launch/succeeded", handle: Number(record.handle), sceneId });
          if (stateRef.current.wheel) send("osf.animation.requestClose");
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
    if (standalone) {
      dispatch({ type: "runtime/ready" });
      dispatch({ type: "nearby/received", kind: "actor", targets: normalizeNearby(MOCK_ACTORS) });
      dispatch({ type: "nearby/received", kind: "furniture", targets: normalizeNearby(MOCK_ANCHORS) });
      requestCatalog(true);
      showNotice("info", "Standalone mode. Snapshot catalog; pick/scan/launch are stubbed. W = animation wheel · B = backdrop.");
      window.mockOpenWheel = (withTarget = true) => (bridge as StandaloneBridge).openWheel(withTarget);
      if (new URLSearchParams(location.search).has("wheel")) window.setTimeout(() => window.mockOpenWheel?.(new URLSearchParams(location.search).get("wheel") !== "solo"), 0);
    } else requestCatalog(true);
    return () => { unsubscribe(); bridge.dispose(); if (catalogTimer.current) clearTimeout(catalogTimer.current); if (libraryTimer.current) clearTimeout(libraryTimer.current); if (noticeTimer.current) clearTimeout(noticeTimer.current); if (padHeld.current.timer) clearTimeout(padHeld.current.timer); };
  }, []);

  useEffect(() => {
    const selectedId = validSelection(state);
    if (selectedId !== state.selectedId) dispatch({ type: "selection/changed", sceneId: selectedId });
  }, [state.catalog, state.library, state.mode, state.filters, state.allSpecies, state.browseAll, state.libCustomOnly, state.cast, state.furniture, state.anchorMatch]);

  useEffect(() => {
    document.body.classList.toggle("wheel-mode", !!state.wheel);
    document.body.classList.toggle("live-mode", state.minimized);
  }, [state.wheel, state.minimized]);

  const commands = useMemo<BrowserCommands>(() => ({
    refresh: () => requestCatalog(true),
    setMode: (mode) => { dispatch({ type: "mode/changed", mode }); if (mode === "library" && !stateRef.current.libraryReceived) { showNotice("info", "Loading the animation library…"); requestLibrary(true); } },
    selectScene: (sceneId) => dispatch({ type: "selection/changed", sceneId }),
    setSearch: (search) => dispatch({ type: "filter/search", search: search.trim().toLowerCase() }),
    toggleDebug: () => dispatch({ type: "filter/debug" }),
    toggleBrowseAll: () => dispatch({ type: "browse/all" }),
    toggleSpecies: () => dispatch({ type: "filter/species" }),
    toggleStep: (step) => dispatch({ type: "step/toggled", step }),
    toggleMarkers: () => dispatch({ type: "markers/toggled" }),
    scan: (kind) => { showNotice("info", `Scanning nearby ${kind === "furniture" ? "furniture" : "actors"}…`); send("osf.animation.scanNearby", { kind, sceneId: stateRef.current.selectedId || "" }); },
    pick: (slot) => send("osf.animation.pickCrosshair", { slot }),
    toggleActor: (token) => { const actor = stateRef.current.nearbyActors.find((candidate) => candidate.token === token); if (actor) dispatch({ type: "cast/toggled", member: { token, name: actor.name, distance: actor.distance, species: actor.species || "human" } }); },
    togglePlayer: () => dispatch({ type: "cast/toggled", member: PLAYER_CAST }),
    removeMember: (index) => dispatch({ type: "cast/removed", index }),
    moveMember: (index, delta) => dispatch({ type: "cast/moved", from: index, to: index + delta, after: delta > 0 }),
    reorderMember: (from, to, after) => dispatch({ type: "cast/moved", from, to, after }),
    toggleAnchor: (token) => { const current = stateRef.current; if (current.furniture?.token === token) dispatch({ type: "anchor/cleared" }); else { const anchor = current.nearbyFurniture.find((candidate) => candidate.token === token); if (anchor) { dispatch({ type: "anchor/selected", anchor: { token, name: anchor.name, distance: anchor.distance } }); send("osf.animation.anchorMatch", { token }); } } },
    clearAnchor: () => dispatch({ type: "anchor/cleared" }),
    toggleLibraryGroup: (key) => dispatch({ type: "library/group", key }),
    toggleSceneGroup: (key, open) => dispatch({ type: "scene/group", key, open }),
    toggleLibraryShowAll: () => dispatch({ type: "library/showAll" }),
    toggleLibraryFull: () => dispatch({ type: "library/full" }),
    toggleLibraryCustomOnly: () => dispatch({ type: "library/customOnly" }),
    toggleBriefAnimations: () => dispatch({ type: "brief/fullAnimations" }),
    toggleOptions: () => dispatch({ type: "brief/options" }),
    setOption: (field, value) => dispatch({ type: "brief/option", field, value }),
    launch: (stageIndex) => {
      const current = stateRef.current;
      const scene = sceneById(current, current.selectedId);
      if (!scene) return;
      const options: Record<string, unknown> = { strip: Number(current.opts.strip), lockPlayer: Number(current.opts.lock), camera: current.opts.camera, speed: Number(current.opts.speed) };
      if (Number.isInteger(stageIndex) && Number(stageIndex) > 0) options.stage = stageIndex;
      const fields: Record<string, unknown> = { sceneId: scene.id, castTokens: current.cast.map((member) => member.token), opts: options };
      const roleNames = scene.roles.map((role) => role.name);
      if (roleNames.length === current.cast.length && roleNames.every((name) => name && !/^role \d+$/i.test(name))) fields.roleNames = roleNames;
      if (scene.requiresFurniture && current.furniture) fields.furnitureToken = current.furniture.token;
      showNotice("info", `Launching "${Number(stageIndex) > 0 ? `${scene.title} · ${stageLabel(scene, Number(stageIndex))}` : scene.title}"…`);
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

  const debugCommands = useMemo<DevCommands>(() => ({
    version: (mode: VersionDebugState) => dispatch({
      type: "plugin/received",
      plugin: mode === "none" ? { plugin: "OSF Animation", version: "1.0.0" } : {
        plugin: "OSF Animation",
        version: "1.0.0",
        ui: { name: "OSF UI", version: mode === "old" ? "1.0.0" : "1.1.0", tested: "1.1.0", outdated: mode === "old", nexusUrl: "https://www.nexusmods.com/starfield/mods/17711" },
      },
    }),
    wheel: (config: WheelDebugState | null) => {
      if (!config) {
        requestCatalog(true);
        window.setTimeout(() => { if (stateRef.current.wheel) send("osf.animation.wheel.get", { tagPrefix: stateRef.current.wheel.tagPrefix }); }, 80);
        return;
      }
      const base = MOCK_CATALOG.filter((value) => isRecord(value) && Array.isArray(value.tags) && value.tags.some((tag: unknown) => String(tag).startsWith("player.emote.")));
      const raw = Array.from({ length: config.count }, (_, index) => {
        const source = base[index % base.length] as Record<string, unknown>;
        const cycle = Math.floor(index / base.length) + 1;
        return { ...source, id: cycle === 1 ? source.id : `${source.id}.${index}`, title: cycle === 1 ? source.title : `${source.title} ${cycle}`, wheelCustomized: config.pins, pinned: config.pins && index < 3 ? 3 - index : 0 };
      });
      const scenes = normalizeCatalog(raw);
      dispatch({ type: "catalog/received", scenes });
      if (!stateRef.current.wheel) return;
      const ordered = config.pins ? scenes.slice(0, 3).reverse() : scenes;
      const entries = ordered.slice(0, WHEEL_MAX).map((scene) => ({ scene: scene.id, stage: null, title: scene.title, detail: scene.title, key: wheelKey(scene.id, null) }));
      dispatch({ type: "wheel/debug", entries, customized: config.pins, received: !config.loading, target: config.target ? { token: 601, name: "Sarah Morgan" } : null, error: config.error ? "No room in front of the actor (debug)." : "" });
    },
  }), [requestCatalog, send]);

  return { state, commands, debugCommands, standalone };
}
