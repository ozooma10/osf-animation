import type { BrowserAction } from "./actions";
import { PLAYER_CAST, type BrowserState, type CastMember } from "./state";

function moveMember(members: CastMember[], from: number, to: number, after = false): CastMember[] {
  if (from < 0 || from >= members.length || to < 0 || to >= members.length) return members;
  const result = members.slice();
  const [member] = result.splice(from, 1);
  let destination = to - (from < to ? 1 : 0) + (after ? 1 : 0);
  destination = Math.max(0, Math.min(result.length, destination));
  if (destination === from) return members;
  result.splice(destination, 0, member);
  return result;
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
      return { ...state, lastHandle: action.handle, lastSceneId: action.sceneId, minimized: !state.wheel };
    case "launch/failed":
      return state.wheel
        ? { ...state, wheel: { ...state.wheel, launching: "", error: action.error } }
        : state;
    case "cast/replaced":
      return { ...state, cast: action.members, stepOpen: { ...state.stepOpen, cast: false } };
    case "cast/toggled": {
      const index = state.cast.findIndex((member) => member.token === action.member.token);
      const cast = index >= 0
        ? state.cast.filter((_, memberIndex) => memberIndex !== index)
        : action.member.kind === "player" ? [PLAYER_CAST, ...state.cast] : [...state.cast, action.member];
      return { ...state, cast, stepOpen: index < 0 ? { ...state.stepOpen, cast: false } : state.stepOpen };
    }
    case "cast/removed":
      return { ...state, cast: state.cast.filter((_, index) => index !== action.index) };
    case "cast/moved": {
      const cast = moveMember(state.cast, action.from, action.to, action.after);
      return cast === state.cast ? state : { ...state, cast };
    }
    case "nearby/received":
      return action.kind === "actor"
        ? { ...state, nearbyActors: action.targets }
        : { ...state, nearbyFurniture: action.targets };
    case "anchor/selected":
      return {
        ...state,
        furniture: action.anchor,
        anchorMatch: null,
        libShowAll: false,
        stepOpen: { ...state.stepOpen, anchor: false },
      };
    case "anchor/cleared":
      return { ...state, furniture: null, anchorMatch: null };
    case "anchor/matched":
      return state.furniture?.token === action.token
        ? { ...state, anchorMatch: { token: action.token, ids: action.ids } }
        : state;
    case "selection/changed":
      return { ...state, selectedId: action.sceneId, briefFullAnims: false };
    case "mode/changed":
      return { ...state, mode: action.mode };
    case "filter/search":
      return { ...state, filters: { ...state.filters, search: action.search } };
    case "filter/debug":
      return { ...state, filters: { ...state.filters, debugMode: !state.filters.debugMode } };
    case "filter/species":
      return { ...state, allSpecies: !state.allSpecies };
    case "browse/all":
      return { ...state, browseAll: !state.browseAll };
    case "library/showAll":
      return { ...state, libShowAll: !state.libShowAll };
    case "library/full":
      return { ...state, libFull: !state.libFull };
    case "library/customOnly":
      return { ...state, libCustomOnly: !state.libCustomOnly };
    case "library/group": {
      const libOpen = new Set(state.libOpen);
      if (libOpen.has(action.key)) libOpen.delete(action.key);
      else libOpen.add(action.key);
      return { ...state, libOpen };
    }
    case "scene/group": {
      const scnOpen = new Map(state.scnOpen);
      scnOpen.set(action.key, action.open);
      return { ...state, scnOpen };
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
    case "wheel/error":
      return state.wheel ? { ...state, wheel: { ...state.wheel, launching: "", error: action.error } } : state;
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
      return { ...state, wheel: null, mode: "scenes", minimized: false, visibilitySerial: state.visibilitySerial + 1 };
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
