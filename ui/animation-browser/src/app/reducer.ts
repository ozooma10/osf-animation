import { diffImportReports } from "../model";
import type { BrowserAction } from "./actions";
import { PLAYER_CAST, type BrowserState, type CastMember } from "./state";

function moveMember(members: CastMember[], from: number, to: number, after = false): CastMember[] {
  // Dropping a row on itself is a cancel, not a reorder: without this, dropping on the
  // lower half of the SAME row (after=true) computed destination = from + 1 and swapped
  // the member with its neighbour — silently rebinding roles (cast order is role order).
  if (from === to) return members;
  if (from < 0 || from >= members.length || to < 0 || to >= members.length) return members;
  const result = members.slice();
  const [member] = result.splice(from, 1);
  let destination = to - (from < to ? 1 : 0) + (after ? 1 : 0);
  destination = Math.max(0, Math.min(result.length, destination));
  if (destination === from) return members;
  result.splice(destination, 0, member);
  return result;
}

// Shallow element-wise equality over arrays of flat objects (marker/indicator payloads).
function sameItems<T extends object>(a: readonly T[], b: readonly T[]): boolean {
  if (a === b) return true;
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    const x = a[i] as Record<string, unknown>;
    const y = b[i] as Record<string, unknown>;
    const keys = Object.keys(x);
    if (keys.length !== Object.keys(y).length) return false;
    for (const k of keys) {
      if (x[k] !== y[k]) return false;
    }
  }
  return true;
}

export function browserReducer(state: BrowserState, action: BrowserAction): BrowserState {
  switch (action.type) {
    case "runtime/ready":
      return { ...state, ready: true };
    case "catalog/requested":
      return { ...state, catalogReceived: false };
    case "library/requested":
      return { ...state, libraryReceived: false };
    case "plugin/received":
      // A full web-view reload can happen after OSF UI's one-shot runtime.ready
      // handshake. The view requests the catalog on every mount, and this version
      // reply comes directly from OSF Animation, so it is equally authoritative
      // proof that the engine is connected.
      return { ...state, ready: true, plugin: action.plugin };
    case "catalog/received":
      return {
        ...state,
        // Keep the catalog reply independently sufficient in case the preceding
        // version message is ever dropped or reordered by the host.
        ready: true,
        catalogReceived: true,
        catalog: action.scenes,
        wheelCustomized: action.scenes.some((scene) => scene.wheelCustomized),
      };
    case "library/received":
      return {
        ...state,
        ready: true,
        libraryReceived: true,
        library: action.scenes,
        wheelCustomized: state.wheelCustomized || action.scenes.some((scene) => scene.wheelCustomized),
      };
    case "active/received": {
      const lastStillActive = !state.lastHandle || action.scenes.some((scene) => scene.handle === state.lastHandle);
      return {
        ...state,
        active: action.scenes,
        lastHandle: lastStillActive ? state.lastHandle : 0,
        minimized: lastStillActive ? state.minimized : false,
        mode: state.mode === "active" && action.scenes.length === 0 ? "scenes" : state.mode,
      };
    }
    case "launch/succeeded":
      return {
        ...state,
        lastHandle: action.handle,
        lastSceneId: action.sceneId,
        minimized: action.inspect ? false : !state.wheel && action.afterLaunch === "minimize",
        mode: action.inspect ? "active" : state.mode,
      };
    case "launch/failed":
      return state.wheel
        ? { ...state, wheel: { ...state.wheel, launching: "", error: action.error } }
        : state;
    case "settings/received": {
      const preferences = { ...state.preferences, ...action.preferences };
      // Re-seed a START OVERRIDES field only when this payload actually carries its key:
      // an unrelated preference write (the CUSTOM ONLY / FULL DETAIL chips) must not
      // discard per-launch overrides the user just set. The authoritative full
      // settings.data payload carries every key, so it still re-seeds everything.
      const carried = action.preferences;
      return {
        ...state,
        preferences,
        filters: { ...state.filters, debugMode: preferences.authorDetails },
        libFull: preferences.libraryDetail === "full",
        libCustomOnly: preferences.librarySource === "custom",
        opts: {
          strip: "strip" in carried ? preferences.strip : state.opts.strip,
          lock: "lock" in carried ? preferences.lock : state.opts.lock,
          camera: "camera" in carried ? preferences.camera : state.opts.camera,
          speed: "speed" in carried ? preferences.speed : state.opts.speed,
        },
      };
    }
    case "settings/open":
      // The two full-panel surfaces cover the same console body, so opening one closes the other.
      return { ...state, settingsOpen: action.open, importsOpen: action.open ? false : state.importsOpen };
    case "imports/open":
      return { ...state, importsOpen: action.open, settingsOpen: action.open ? false : state.settingsOpen };
    case "imports/requested":
      return { ...state, importsReceived: false };
    case "imports/received":
      return { ...state, importsReceived: true, imports: action.files, importTotals: action.totals };
    case "imports/expanded": {
      const importsExpanded = new Set(state.importsExpanded);
      if (action.open) importsExpanded.add(action.path);
      else importsExpanded.delete(action.path);
      return { ...state, importsExpanded };
    }
    case "imports/filter":
      return { ...state, importsFilter: action.filter };
    case "imports/search":
      return { ...state, importsSearch: action.search };
    case "imports/reloadStarted":
      return {
        ...state,
        importReload: { ...state.importReload, status: "running", error: "", newProblemKeys: new Set() },
      };
    case "imports/reloadSucceeded": {
      const delta = diffImportReports(state.imports, action.files);
      return {
        ...state,
        importsReceived: true,
        imports: action.files,
        importTotals: action.totals,
        importReload: {
          status: "success",
          completedAt: action.completedAt,
          durationMs: action.durationMs,
          scenes: action.scenes,
          error: "",
          delta,
          newProblemKeys: new Set(delta.newProblems.map((problem) => problem.key)),
        },
      };
    }
    case "imports/reloadFailed":
      return {
        ...state,
        importReload: { ...state.importReload, status: "error", completedAt: action.completedAt,
          durationMs: action.durationMs, error: action.error, newProblemKeys: new Set() },
      };
    case "imports/viewContent":
      return {
        ...state, importsOpen: false, mode: "scenes", browseKind: "all", browseAll: true,
        allSpecies: true, libFull: true, libCustomOnly: false,
        filters: { ...state.filters, search: action.path.toLowerCase(), debugMode: true },
      };
    case "cast/replaced":
      return { ...state, cast: action.members };
    case "cast/toggled": {
      const index = state.cast.findIndex((member) => member.token === action.member.token);
      const cast = index >= 0
        ? state.cast.filter((_, memberIndex) => memberIndex !== index)
        : action.member.kind === "player" ? [PLAYER_CAST, ...state.cast] : [...state.cast, action.member];
      const removedLocation = index >= 0 && state.locationMode === "actor" && state.locationToken === action.member.token;
      // Deliberately leaves stepOpen alone: collapsing CREW on every addition
      // made armed world-picking feel broken (each click folded the panel).
      return {
        ...state,
        cast,
        locationMode: removedLocation ? "cast" : state.locationMode,
        locationToken: removedLocation ? null : state.locationToken,
      };
    }
    case "cast/removed": {
      const removed = state.cast[action.index];
      const removedLocation = !!removed && state.locationMode === "actor" && state.locationToken === removed.token;
      return {
        ...state,
        cast: state.cast.filter((_, index) => index !== action.index),
        locationMode: removedLocation ? "cast" : state.locationMode,
        locationToken: removedLocation ? null : state.locationToken,
      };
    }
    case "cast/moved": {
      const cast = moveMember(state.cast, action.from, action.to, action.after);
      return cast === state.cast ? state : { ...state, cast };
    }
    case "nearby/received":
      return action.kind === "actor"
        ? { ...state, nearbyActors: action.targets }
        : { ...state, nearbyFurniture: action.targets };
    case "indicators/received":
      // Element-wise bail-out (same idea as cast/moved and pickTargets/received): the
      // ~12.5 Hz projectActors poll re-rendered the whole tree even when nothing moved.
      return sameItems(state.actorIndicators, action.items) ? state : { ...state, actorIndicators: action.items };
    case "pickTargets/received":
      // Replies are tagged with the slot they were polled for, so a late
      // in-flight actor reply can't paint markers into furniture mode (or
      // vice versa), and anything after cancel is dropped.
      return state.pickMode === action.slot ? { ...state, pickTargets: action.items } : state;
    case "pick/armed":
      return { ...state, pickMode: action.kind, pickTargets: [] };
    case "pick/cancelled":
      return { ...state, pickMode: null, pickTargets: [] };
    case "anchor/selected":
      return {
        ...state,
        furniture: action.anchor,
        locationMode: "furniture",
        locationToken: action.anchor.token,
        anchorMatch: null,
      };
    case "anchor/cleared":
      return {
        ...state,
        furniture: null,
        anchorMatch: null,
        locationMode: state.locationMode === "furniture" ? "cast" : state.locationMode,
        locationToken: state.locationMode === "furniture" ? null : state.locationToken,
      };
    case "anchor/matched":
      return state.furniture?.token === action.token
        ? { ...state, anchorMatch: { token: action.token, ids: action.ids } }
        : state;
    case "location/selected":
      return {
        ...state,
        locationMode: action.mode,
        locationToken: action.mode === "actor" || action.mode === "furniture" ? action.token ?? null : null,
      };
    case "selection/changed":
      return {
        ...state,
        selectedId: action.sceneId,
        selectedStage: action.stage ?? null,
        briefFullAnims: false,
      };
    case "mode/changed":
      return {
        ...state,
        mode: action.mode,
        lastBrowseMode: action.mode === "wheel" ? state.lastBrowseMode : action.mode,
        settingsOpen: false,
        importsOpen: false,
      };
    case "browser/opened":
      return {
        ...state,
        mode: action.mode,
        lastBrowseMode: action.mode,
        settingsOpen: false,
        importsOpen: false,
        ...(action.resetBrowsing ? {
          filters: { ...state.filters, search: "" },
          browseAll: false,
          browseKind: "all",
          allSpecies: false,
          libOpen: new Map<string, boolean>(),
        } : {}),
      };
    case "filter/search":
      return { ...state, filters: { ...state.filters, search: action.search } };
    case "filter/species":
      return { ...state, allSpecies: !state.allSpecies };
    case "browse/all":
      return { ...state, browseAll: !state.browseAll };
    case "browse/kind":
      return { ...state, browseKind: action.kind };
    case "library/group": {
      const libOpen = new Map(state.libOpen);
      libOpen.set(action.key, action.open);
      return { ...state, libOpen };
    }
    case "brief/fullAnimations":
      return { ...state, briefFullAnims: !state.briefFullAnims };
    case "brief/options":
      return { ...state, optsOpen: !state.optsOpen };
    case "brief/option":
      return { ...state, opts: { ...state.opts, [action.field]: action.value } } as BrowserState;
    case "markers/toggled":
      return { ...state, markersOpen: !state.markersOpen };
    case "step/toggled":
      return { ...state, stepOpen: { ...state.stepOpen, [action.step]: !state.stepOpen[action.step] } };
    case "minimized/changed":
      return { ...state, minimized: action.minimized };
    case "scene/stopped":
      return {
        ...state,
        active: state.active?.filter((scene) => scene.handle !== action.handle) ?? null,
        lastHandle: state.lastHandle === action.handle ? 0 : state.lastHandle,
        minimized: state.lastHandle === action.handle ? false : state.minimized,
      };
    case "wheel/entered": {
      const wheel = state.wheel
        ? { ...state.wheel, tagPrefix: action.tagPrefix, target: action.target }
        : {
            tagPrefix: action.tagPrefix,
            target: action.target,
            focus: 0,
            error: "",
            launching: "",
            received: false,
            requested: false,
            entries: [],
          };
      return { ...state, mode: "wheel", wheel };
    }
    case "wheel/exited":
      return { ...state, mode: "scenes", wheel: null };
    case "wheel/requested":
      return state.wheel ? { ...state, wheel: { ...state.wheel, requested: true } } : state;
    case "wheel/received":
      return state.wheel
        ? { ...state, wheelCustomized: action.customized, wheel: { ...state.wheel, entries: action.entries, received: true } }
        : state;
    case "wheel/focused":
      return state.wheel ? { ...state, wheel: { ...state.wheel, focus: action.focus } } : state;
    case "wheel/launching":
      return state.wheel ? { ...state, wheel: { ...state.wheel, error: "", launching: action.key } } : state;
    case "wheel/debug":
      return state.wheel ? {
        ...state,
        wheelCustomized: action.customized,
        wheel: { ...state.wheel, entries: action.entries, received: action.received, target: action.target, error: action.error, launching: "", focus: 0 },
      } : state;
    case "wheel/customized":
      return { ...state, wheelCustomized: true, catalog: action.catalog, library: action.library };
    case "wheel/reset":
      return { ...state, wheelCustomized: false, catalog: action.catalog, library: action.library };
    case "visibility/hidden":
      return { ...state, wheel: null, mode: "scenes", settingsOpen: false, importsOpen: false, minimized: false, pickMode: null, actorIndicators: [], pickTargets: [], viewVisible: false, visibilitySerial: state.visibilitySerial + 1 };
    case "visibility/shown":
      return { ...state, viewVisible: true };
    case "seeded/remembered": {
      const seededTokens = new Set(state.seededTokens);
      seededTokens.add(action.token);
      return { ...state, seededTokens };
    }
    case "notice/show":
      return { ...state, notice: { kind: action.kind, text: action.text, serial: state.notice.serial + 1 } };
    case "notice/clear":
      return state.notice.serial === action.serial
        ? { ...state, notice: { kind: "", text: "", serial: state.notice.serial } }
        : state;
  }
}
