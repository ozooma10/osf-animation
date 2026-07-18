// OSF Animation Browser. NASA-punk maintenance-HUD console.
// Guided pre-flight flow: 1 · CAST (pick actors) → 2 · ANCHOR (optional furniture, shows what
// it unlocks) → BROWSE (only what plays with the current selection, plus the vanilla library).
// Wired to the OSF UI bridge contract (protocol 1.0): only JSON text crosses window.osfui.
"use strict";

const PLAYER_TOKEN = -1;
const PLAYER_CAST = { token: PLAYER_TOKEN, name: "Player", kind: "player", species: "human" };

const state = {
  ready: false,
  catalog: [],
  wheelCustomized: false,  // false = derive installed defaults; true = `pinned` is the explicit loadout
  allUnlisted: false,
  selectedId: null,
  // Ordered cast. Role/slot binding is BY THIS ORDER (member 0 -> first role, etc.), so the list is
  // reorderable (drag, or Alt+↑/↓). The player is an inline member (kind "player") that may be
  // dropped for NPC-only scenes (machinima / vignettes) or moved out of the lead slot.
  cast: [PLAYER_CAST],
  furniture: null,
  nearbyActors: [],
  nearbyFurniture: [],
  lastHandle: 0,
  lastSceneId: "",
  // Live scenes (osf.animation.activeScenes push): several can run at once — NPC-only
  // scenes keep running after the browser closes, so a reopen lists them here (the
  // ACTIVE tab is their management surface). null until the first push (an older DLL
  // never sends one; activeScenes() then falls back to a single lastHandle row).
  active: null,
  opts: { strip: "-1", lock: "-1", camera: "", speed: "1" },
  optsOpen: false,    // START OVERRIDES disclosure in the brief footer (collapsed by default)
  filters: { search: "", authorMode: false },
  catalogReceived: false,
  // Which anchor-bound scenes the KEYED furniture actually fits (osf.anchorMatch reply).
  // null until known; browse falls back to "any anchor present" while the reply is in flight.
  anchorMatch: null,  // { token, ids: Set<string> }
  browseAll: false,   // scenes mode: false = playable now only, true = everything
  allSpecies: false,  // override the per-cast species filter (show human + every creature's animations)
  // Reference-library lane (osf.library.*): the generated vanilla packs. Static after load,
  // fetched once on demand when the LIBRARY mode is first opened, then cached for the session.
  mode: "scenes",  // "scenes" | "library" | "wheel" (transient — see EMOTE WHEEL)
  // Emote-wheel context while mode === "wheel" (osf.mode push from the native OpenWheel):
  // { tagPrefix, target: {token,name}|null, focus, error, launching }. null otherwise.
  wheel: null,
  // Live mode: the console/brief collapse to a small floating live bar so the world (the
  // running scene) is the whole screen. Entered automatically on a successful launch, or
  // manually via the slate minimize button; exited by EXPAND/stop. Never survives a hide.
  minimized: false,
  library: [],
  libraryReceived: false,
  libOpen: new Set(),  // expanded library pack groups
  libShowAll: false,   // library: false = focus on sets fitting the keyed anchor, true = all
  libFull: false,      // library: false = clean tier (pose/loopable clips only), true = the full dump
  briefFullAnims: false,  // brief: library sets fold transition/partial stages behind a count
  markersOpen: false,  // anchor step: the collapsed "AI markers" group (invisible idle markers)
  // Collapsible pre-flight steps: once cast/anchor are set they fold to a summary line so
  // the browse list gets the dock's height back (the rail otherwise eats ~half the column).
  stepOpen: { cast: true, anchor: true },
  // Tokens already seeded from an open-time crosshair target (osf.animation.openTarget), so a
  // re-push (or a reopen with the same target) never re-adds one the user has since dropped.
  seededTokens: new Set(),
};

const $ = (id) => document.getElementById(id);

/* =========================================================================
   BRIDGE  (protocol 1.0 — osf.animation.* over window.osfui)
   ========================================================================= */
function bridgeAvailable() {
  return typeof window.osfui === "object" && typeof window.osfui.postMessage === "function";
}

function send(command, fields = {}) {
  const msg = JSON.stringify({ type: "ui.command", payload: { command, ...fields } });
  if (bridgeAvailable()) window.osfui.postMessage(msg);
  else { console.log("(standalone) ->", msg); mockNative(command, fields); }
}

/* =========================================================================
   ORBIT DRAG  (world-area click-drag steers the native scene-orbit camera)
   While the overlay is open OSF UI consumes ALL game input, so the native
   orbit camera never sees the mouse. The view forwards WORLD-AREA drags over
   the bridge instead: LMB-drag anywhere outside the console/brief = steer,
   wheel there = zoom (osf.orbit {dx,dy,wheel} in CSS px / notches, batched
   per animation frame). Interactions that start ON the panels stay pure UI.
   ========================================================================= */
const orbit = { dragging: false, x: 0, y: 0, dx: 0, dy: 0, wheel: 0, queued: false };

function orbitWorldTarget(e) {
  if (state.wheel) return false;  // wheel mode: the world area is the wheel's cancel/pick surface
  return !(e.target instanceof Element && e.target.closest(".console, .brief, .livebar"));
}

function orbitFlush() {
  orbit.queued = false;
  if (!bridgeAvailable()) return;  // standalone: nothing to steer, don't spam the mock
  if (orbit.dx || orbit.dy || orbit.wheel) {
    send("osf.animation.orbit", { dx: orbit.dx, dy: orbit.dy, wheel: orbit.wheel });
    orbit.dx = orbit.dy = orbit.wheel = 0;
  }
}

function orbitQueue() {
  if (!orbit.queued) { orbit.queued = true; requestAnimationFrame(orbitFlush); }
}

let catalogTimer = null;
let catalogTries = 0;
const CATALOG_MAX_TRIES = 20;

function requestCatalog(fresh) {
  if (fresh) { state.catalogReceived = false; catalogTries = 0; }
  clearTimeout(catalogTimer);
  if (state.catalogReceived) return;
  send("osf.animation.catalog.get");
  if (catalogTries < CATALOG_MAX_TRIES) {
    catalogTries++;
    catalogTimer = setTimeout(() => requestCatalog(false), 1200);
  } else {
    notice("err", "No response from OSF Animation. Load a save and make sure OSF UI is present.");
  }
}

let libraryTimer = null;
let libraryTries = 0;
const LIBRARY_MAX_TRIES = 5;

function requestLibrary(fresh) {
  if (fresh) { state.libraryReceived = false; libraryTries = 0; }
  clearTimeout(libraryTimer);
  if (state.libraryReceived) return;
  send("osf.animation.library.get");
  if (libraryTries < LIBRARY_MAX_TRIES) {
    libraryTries++;
    libraryTimer = setTimeout(() => requestLibrary(false), 1500);
  } else {
    notice("err", "No library data from OSF Animation (older plugin build?).");
  }
}

function onNativeMessage(jsonText) {
  let msg;
  try { msg = JSON.parse(jsonText); } catch { return; }
  const { type, payload } = msg;
  switch (type) {
    case "runtime.ready": handleReady(payload); break;
    case "osf.animation.catalog.data": handleCatalog(payload); break;
    case "osf.animation.library.data": handleLibrary(payload); break;
    case "osf.animation.pick": handlePick(payload); break;
    case "osf.animation.openTarget": handleOpenTarget(payload); break;
    case "osf.animation.scanResults": handleScanResults(payload); break;
    case "osf.animation.anchorMatch": handleAnchorMatch(payload); break;
    case "osf.animation.launchResult": handleLaunchResult(payload); break;
    case "osf.animation.activeScenes": handleActiveScenes(payload); break;
    // Native mode switch (OpenWheel): "wheel" enters the emote wheel; anything else restores
    // the console. Delivered pre-paint by OSF UI's message queue (bridge MINOR >= 2) and
    // replayed on osf.opened while a wheel open is pending, so it must be idempotent.
    case "osf.animation.mode":
      if (payload && payload.mode === "wheel") enterWheel(payload);
      else exitWheel();
      break;
    // The runtime tells the focused view when the overlay shows/hides; report both so the
    // plugin's first-run "press F10" hint can count real opens, and so the scene-orbit camera
    // knows a cursor is on screen (visible = LMB-drag steers the orbit; hidden = free-look).
    case "ui.visibility":
      if (!(payload && payload.visible)) {
        orbit.dragging = false;  // never carry a drag across a hide
        // Drop deltas a queued rAF flush hasn't sent yet: flushed after the close relay they'd
        // re-engage the native browse orbit on a closed browser (stuck orbit camera).
        orbit.dx = orbit.dy = orbit.wheel = 0;
        exitWheel();             // wheel mode never survives a hide — a reopen shows the console
        setMinimized(false);     // ditto live mode: F10 reopen means "browse", show the console
      }
      send(payload && payload.visible ? "osf.animation.opened" : "osf.animation.closed");
      break;
    // The runtime rejected something we sent (protocol 1.0: stable machine `code`
    // plus a human `message`). Surface it — a silently dropped command reads as
    // "the button did nothing".
    case "ui.error":
      notice("err", `Bridge rejected a message: ${(payload && (payload.message || payload.code)) || "unknown error"}`);
      break;
    default: break;
  }
}

window.osfui = window.osfui || {};
window.osfui.onMessage = onNativeMessage;

function handleReady(p) {
  const bv = p && p.bridgeVersion;
  // Require a bridge to be present, but never gate on the protocol version
  // string — it is informational, and the contract evolves additively (new
  // capabilities, minor/major bumps). Feature-detect if a specific surface is
  // ever needed; a hard version check here would reject every future OSF UI.
  if (!bv) {
    setLamp("off");
    $("statusText").textContent = `unsupported bridge ${bv || "?"}`;
    notice("err", `This view needs the OSF UI bridge; runtime reports ${bv || "?"}.`);
    return;
  }
  state.ready = true;
  setLamp("ok");
  $("statusText").textContent = `${(p.plugin || "OSF").toUpperCase()} v${p.version || "?"} · stage online`;
  notice("ok", `Bridge online. Protocol ${bv}.`);
  requestCatalog(true);
}

function handleCatalog(list) {
  state.catalogReceived = true;
  clearTimeout(catalogTimer);
  state.catalog = Array.isArray(list) ? list.map(safeNormalize).filter(Boolean) : [];
  state.wheelCustomized = state.catalog.some((s) => s.wheelCustomized);
  const scenes = sceneCatalog();
  state.allUnlisted = scenes.length > 0 && scenes.every((s) => s.unlisted);
  ensureSelection();
  renderAll();
}

function handleLibrary(list) {
  state.libraryReceived = true;
  clearTimeout(libraryTimer);
  state.library = Array.isArray(list) ? list.map(safeNormalize).filter(Boolean) : [];
  for (const s of state.library) {
    s.library = true;
    // Library search should find individual animations, not just set names.
    s.stageHay = (s.stages || []).map((st) => st.name).join(" ").toLowerCase();
  }
  ensureSelection();
  renderAll();
}

function handlePick(p) {
  if (!p || !p.valid || !p.token) {
    notice("err", `No ${(p && p.slot) || "target"} was under the crosshair when the browser opened — aim first, then open, or use SCAN.`);
    return;
  }
  applyPick(p.slot === "furniture" ? "furniture" : "actor", p.token, p.name, p.distance, p.species);
}

// What the reticle was on when the browser opened (pushed on every open). Aiming at someone is
// a statement of intent, so it seeds the pre-flight: an actor BECOMES the crew (the player is
// only the default when there was no target), furniture keys the anchor. Never clobbers a crew
// or anchor the user has already touched this session — only the untouched default gives way.
function handleOpenTarget(p) {
  if (!p || !p.token || state.seededTokens.has(p.token)) return;
  state.seededTokens.add(p.token);
  if (p.slot === "furniture") {
    if (state.furniture) return;   // user already keyed one — leave it
    keyAnchor(p.token, p.name, p.distance);
  } else {
    const untouched = state.cast.length === 1 && state.cast[0].kind === "player";
    if (!untouched) { applyPick("actor", p.token, p.name, p.distance, p.species); return; }
    state.cast = [{ token: p.token, name: p.name || "actor", distance: p.distance || null, species: p.species || "human" }];
    state.stepOpen.cast = false;
    notice("info", `Crew: ${p.name || "actor"} (your crosshair target).`);
  }
  renderAll();
}

function handleScanResults(p) {
  const kind = p && p.kind === "furniture" ? "furniture" : "actor";
  const items = p && Array.isArray(p.items) ? p.items : [];
  const normalized = items.map((it) => ({
    token: it.token,
    name: it.name || "(unnamed)",
    formId: it.formId || 0,
    distance: typeof it.distance === "number" ? it.distance : null,
    isActor: !!it.isActor,
    species: it.species ? String(it.species).toLowerCase() : "",  // actor: skeleton family (creature filtering)
    sceneCount: typeof it.sceneCount === "number" ? it.sceneCount : null,  // furniture: total scenes it unlocks
    customCount: typeof it.customCount === "number" ? it.customCount : null,  // furniture: subset that is custom (authored), not vanilla library
    marker: !!it.marker,  // invisible AI/idle marker — a usable anchor, listed in its own group
  }));
  if (kind === "furniture") {
    state.nearbyFurniture = normalized;
    const markers = normalized.filter((x) => x.marker).length;
    const named = normalized.length - markers;
    notice("info", `${named} usable furniture spot${named === 1 ? "" : "s"} found${markers ? ` + ${markers} AI marker${markers === 1 ? "" : "s"}` : ""}.`);
  } else {
    state.nearbyActors = normalized;
    notice("info", `${normalized.length} nearby actor${normalized.length === 1 ? "" : "s"} found.`);
  }
  renderAll();
}


function handleAnchorMatch(p) {
  if (!p || typeof p.token !== "number") return;
  // Only accept the reply for the anchor that is still keyed (the user may have re-keyed).
  if (!state.furniture || state.furniture.token !== p.token) return;
  state.anchorMatch = { token: p.token, ids: new Set(Array.isArray(p.sceneIds) ? p.sceneIds : []) };
  renderAll();
}

// Authoritative live-scene list, pushed by the DLL on open, launch, stage advance, and
// every scene end (including natural timer/loop ends while the browser is up).
function handleActiveScenes(p) {
  state.active = p && Array.isArray(p.scenes) ? p.scenes : [];
  if (state.lastHandle && !state.active.some((s) => s.handle === state.lastHandle)) {
    // The scene this session launched ended on its own — release the live bar.
    state.lastHandle = 0;
    setMinimized(false);
  }
  renderAll();
}

function handleLaunchResult(p) {
  // Wheel pick: success closes the whole view (host-driven via osf.requestClose — exit lands
  // when the host pushes ui.visibility hide); an error shows in the hub and the wheel stays open.
  if (state.wheel) {
    const launched = state.wheel.launching;
    state.wheel.launching = "";
    if (p && p.ok && p.handle) {
      state.lastHandle = p.handle;
      state.lastSceneId = p.sceneId || launched || "";
      send("osf.animation.requestClose");
    } else {
      state.wheel.error = (p && p.error) || "Launch failed.";
      renderWheel();
    }
    return;
  }
  if (p && p.ok && p.handle) {
    state.lastHandle = p.handle;
    state.lastSceneId = p.sceneId || state.selectedId || "";
    notice("ok", `Playing "${sceneTitle(state.lastSceneId)}" on handle ${p.handle}.`);
    setMinimized(true);  // get the console out of the way — the scene is the point now
  } else {
    notice("err", (p && p.error) || "Launch failed.");
  }
  renderAll();
}

/* =========================================================================
   NORMALIZE  (unchanged — the shape main.js consumes from the registry)
   ========================================================================= */
// One malformed record must never blank the whole catalog: drop it, keep the rest.
function safeNormalize(raw) {
  try { return raw ? normalizeScene(raw) : null; }
  catch (e) { console.warn("OSF: skipped malformed scene", e); return null; }
}

function normalizeScene(raw) {
  const id = String(raw.id || "");
  const actorCount = clampCount(raw.actorCount, raw.roles);
  const genders = Array.isArray(raw.genders) ? raw.genders : [];
  const roles = normalizeRoles(raw.roles, genders, actorCount);
  const tags = Array.isArray(raw.tags) ? raw.tags.map((t) => String(t)) : [];
  const taggedEmote = tags.some((t) => t.toLowerCase().startsWith("player.emote."));
  const presentation = String(raw.presentation || (taggedEmote ? "emote" : "scene")).toLowerCase();
  const requiresFurniture = !!(raw.requiresFurniture || raw.anchorRequired || raw.anchor);
  return {
    id,
    title: String(raw.title || raw.name || id || "Unnamed scene"),
    species: String(raw.species || "human").toLowerCase(),  // skeleton family; drives the per-cast filter
    tags,
    actorCount,
    genders,
    roles,
    requiresFurniture,
    anchors: Array.isArray(raw.anchors) ? raw.anchors.map((a) => String(a)) : [],  // what it anchors to ("Barstool", …)
    unlisted: !!raw.unlisted,
    presentation,
    wheelEligible: raw.wheelEligible == null ? presentation === "emote" : !!raw.wheelEligible,
    wheelDefault: raw.wheelDefault == null ? taggedEmote : !!raw.wheelDefault,
    wheelCustomized: !!raw.wheelCustomized,
    pinned: Math.max(0, Math.trunc(Number(raw.pinned) || 0)),  // 1-based explicit wheel order (0 = absent/default-derived)
    priority: Number.isFinite(Number(raw.priority)) ? Number(raw.priority) : 0,
    weight: Number.isFinite(Number(raw.weight)) ? Number(raw.weight) : 1,
    sourceFile: String(raw.sourceFile || raw.source || ""),
    shape: normalizeShape(raw, actorCount),
    policy: normalizePolicy(raw),
    stages: normalizeStages(raw.stages),
    estSec: numOrNull(raw.estSec),        // summed stage estimates (null = engine hasn't probed the clips yet)
    estPartial: !!raw.estPartial,         // some stage contributed no estimate — treat estSec as "at least"
    openEnded: !!raw.openEnded,           // holds a stage until advanced — wall time is the player's call
  };
}

// Each stage is a browsable animation: { index, name, tags, clipCount, loopSec, estSec, … }.
function normalizeStages(stages) {
  if (!Array.isArray(stages)) return [];
  return stages.map((raw, i) => {
    const st = raw || {};
    return {
      index: Number.isInteger(st.index) ? st.index : i,
      name: String(st.name || ""),
      tags: Array.isArray(st.tags) ? st.tags.map((t) => String(t)) : [],
      clipCount: Number(st.clipCount || 0),
      loopSec: numOrNull(st.loopSec),     // the clip's loop length (the honest per-animation number)
      timerSec: numOrNull(st.timerSec),   // auto-advance timer, if any
      loops: numOrNull(st.loops),         // null = play once, 0 = hold, N = loop count
      openEnded: !!st.openEnded,
      estSec: numOrNull(st.estSec),
    };
  });
}

function numOrNull(v) { const n = Number(v); return v == null || !Number.isFinite(n) ? null : n; }

function clampCount(actorCount, roles) {
  if (Number.isFinite(Number(actorCount)) && Number(actorCount) > 0) return Number(actorCount);
  if (Array.isArray(roles) && roles.length) return roles.length;
  return 0;
}

function normalizeRoles(roles, genders, actorCount) {
  if (Array.isArray(roles) && roles.length) {
    return roles.map((raw, index) => {
      const role = raw || {};
      return {
        name: String(role.name || `role ${index + 1}`),
        gender: String(role.gender || (role.filters && role.filters.gender) || "any"),
        filters: role.filters || {},
        equip: !!role.equip,
      };
    });
  }
  const total = actorCount || genders.length || 0;
  return Array.from({ length: total }, (_, index) => ({
    name: `role ${index + 1}`,
    gender: String(genders[index] || "any"),
    filters: {},
    equip: false,
  }));
}

function normalizeShape(raw, actorCount) {
  if (raw.shape && typeof raw.shape === "object") {
    return {
      kind: String(raw.shape.kind || "linear"),
      stages: Number(raw.shape.stages || raw.shape.stageCount || 0),
      nodes: Number(raw.shape.nodes || raw.shape.nodeCount || 0),
      branches: Number(raw.shape.branches || raw.shape.branchCount || 0),
    };
  }
  const stages = Number(raw.stageCount || raw.linearStageCount || 0);
  const nodes = Number(raw.nodeCount || 0);
  const branches = Number(raw.branchCount || 0);
  return { kind: branches > 0 || nodes > stages ? "graph" : "linear", stages: stages || (actorCount ? 1 : 0), nodes: nodes || (stages || 1), branches };
}

function normalizePolicy(raw) {
  const policy = raw.policy && typeof raw.policy === "object" ? raw.policy : {};
  return {
    stripActors: boolText(policy.stripActors, raw.stripActors, "inherit"),
    lockPlayer: boolText(policy.lockPlayer, raw.lockPlayer, "inherit"),
    fade: boolText(policy.fade, raw.fade, "off"),
    camera: String(policy.camera || raw.camera || "inherit"),
    playerControl: policy.playerControl || raw.playerControl || null,
  };
}

function boolText(primary, fallback, emptyValue) {
  const value = primary !== undefined ? primary : fallback;
  if (value === true) return "on";
  if (value === false) return "off";
  return emptyValue;
}

/* =========================================================================
   STATE MUTATIONS
   ========================================================================= */
function keyAnchor(token, name, distance) {
  state.furniture = { token, name: name || "furniture", distance: distance || null };
  state.anchorMatch = null;               // stale until the engine answers for THIS token
  state.libShowAll = false;               // re-focus the library on what this anchor fits
  state.stepOpen.anchor = false;          // step done — fold it, give the browse list the height back
  send("osf.animation.anchorMatch", { token });     // which anchor-bound scenes does it fit?
  notice("info", `Furniture keyed: ${state.furniture.name}.`);
}

function clearAnchor() {
  state.furniture = null;
  state.anchorMatch = null;
}

function applyPick(slot, token, name, distance, species) {
  if (slot === "furniture") {
    keyAnchor(token, name, distance);
  } else if (state.cast.some((c) => c.token === token)) {
    notice("info", `${name || "Actor"} is already in the crew.`);
  } else {
    state.cast.push(token === PLAYER_TOKEN ? PLAYER_CAST : { token, name: name || "actor", distance: distance || null, species: species || "human" });
    state.stepOpen.cast = false;  // partner added — fold, freeing the browse list's height (mirrors keyAnchor)
    notice("info", `Crew added: ${name || "actor"}.`);
  }
  renderAll();
}

function toggleActor(token) {
  const idx = state.cast.findIndex((m) => m.token === token);
  if (idx >= 0) { state.cast.splice(idx, 1); notice("info", "Crew member removed."); }
  else {
    const a = state.nearbyActors.find((x) => x.token === token);
    if (a) { state.cast.push({ token: a.token, name: a.name, distance: a.distance, species: a.species || "human" }); state.stepOpen.cast = false; notice("info", `Crew added: ${a.name}.`); }
  }
  renderAll();
}

function togglePlayer() {
  const idx = state.cast.findIndex((m) => m.kind === "player");
  if (idx >= 0) { state.cast.splice(idx, 1); notice("info", "Player removed — NPC-only cast."); }
  else { state.cast.unshift(PLAYER_CAST); notice("info", "Player added to the crew."); }  // re-add as the lead
  renderAll();
}

function toggleAnchor(token) {
  if (state.furniture && state.furniture.token === token) { clearAnchor(); notice("info", "Furniture cleared."); }
  else {
    const an = state.nearbyFurniture.find((x) => x.token === token);
    if (an) keyAnchor(an.token, an.name, an.distance);
  }
  renderAll();
}

function removeMember(index) {
  if (index < 0 || index >= state.cast.length) return;
  state.cast.splice(index, 1);
  renderAll();
}

// Move the cast member at `from` so it lands relative to the member at `to`. `after` = drop below
// the target (else above). Role/slot binding follows this order, so this is how the user assigns
// which actor plays which role. Returns true if the order actually changed.
function moveMember(from, to, after) {
  const n = state.cast.length;
  if (from < 0 || from >= n || to < 0 || to >= n) return false;
  const arr = state.cast.slice();
  const [m] = arr.splice(from, 1);
  let idx = to;
  if (from < to) idx -= 1;   // removal shifted the target left by one
  if (after) idx += 1;
  idx = Math.max(0, Math.min(arr.length, idx));
  if (idx === from) return false;   // no net move
  arr.splice(idx, 0, m);
  state.cast = arr;
  return true;
}

// Nudge a member one slot up or down. Drives every reorder path: the ▲/▼ buttons, Alt+↑/↓, and drag.
// scopeSel = the panel to keep focus in (crew list or brief role-map — they mirror each other);
// focusAct = a data-act to re-focus at the new index (the ▲/▼ button) so repeated presses keep working.
function nudgeMember(index, delta, scopeSel, focusAct) {
  const to = index + delta;
  if (to < 0 || to >= state.cast.length) return;
  if (moveMember(index, to, delta > 0)) {
    notice("info", "Cast order updated.");
    renderAll();
    requestAnimationFrame(() => {
      const root = (scopeSel && document.querySelector(scopeSel)) || document;
      // Prefer the same button at its new slot; fall back to the chip (the button may now be a
      // disabled end-stop, e.g. after moving an item to the very top/bottom).
      let el = focusAct ? root.querySelector(`[data-act="${focusAct}"][data-i="${to}"]:not([disabled])`) : null;
      if (!el) el = root.querySelector(`.castline[data-i="${to}"]`);
      if (el) el.focus({ preventScroll: true });
    });
  }
}

// Explicit up/down controls. The RELIABLE reorder path in hosts without HTML5 drag (in-game
// Ultralight) and for mouse/keyboard/gamepad alike — drag is a nice-to-have on top. count =
// number of reorderable members; end-stops are disabled.
function reorderBtns(i, count) {
  if (count < 2) return "";
  const up = `<button class="chip-move" data-act="move-up" data-i="${i}" title="Move up (role earlier)" ${i === 0 ? "disabled" : ""}>▲</button>`;
  const dn = `<button class="chip-move" data-act="move-down" data-i="${i}" title="Move down (role later)" ${i === count - 1 ? "disabled" : ""}>▼</button>`;
  return `<span class="chip-moves">${up}${dn}</span>`;
}

function scanNearby(kind) {
  notice("info", `Scanning nearby ${kind === "furniture" ? "furniture" : "actors"}…`);
  send("osf.animation.scanNearby", { kind, sceneId: state.selectedId || "" });
}

function setMode(mode) {
  if (state.mode === mode) return;
  state.mode = mode;
  if (mode === "library" && !state.libraryReceived) {
    notice("info", "Loading the animation library…");
    requestLibrary(true);
  }
  ensureSelection();
  renderAll();
}

function isEmote(s) { return !!s && s.presentation === "emote"; }
function sceneCatalog() { return state.catalog.filter((s) => !isEmote(s)); }
function emoteCatalog() { return state.catalog.filter(isEmote); }
function animationList() { return [...emoteCatalog(), ...state.library]; }
function activeList() {
  if (state.mode === "library") return animationList();
  if (state.mode === "scenes") return sceneCatalog();
  return state.catalog;  // ACTIVE may contain either a scene or an emote wrapper
}

function applySelection(id) { state.selectedId = id; state.briefFullAnims = false; }
function selectScene(id) { applySelection(id); renderAll(); }
function sceneById(id) { return state.catalog.find((s) => s.id === id) || state.library.find((s) => s.id === id) || null; }
function sceneTitle(id) { const s = sceneById(id); return s ? s.title : (id || "scene"); }
function castTokens() { return state.cast.map((m) => m.token); }
function castMembers() { return state.cast; }
// The live-scene rows to show: the pushed list, or (older DLL / standalone before any
// push) a single row synthesized from the last launch.
function activeScenes() {
  if (state.active) return state.active;
  return state.lastHandle ? [{ handle: state.lastHandle, sceneId: state.lastSceneId, player: true, cast: [] }] : [];
}
// Cast tokens currently in a running scene (player = -1), for the LIVE badges.
function busyTokens() {
  const set = new Set();
  for (const s of activeScenes()) for (const c of s.cast || []) set.add(c.token);
  return set;
}
function hasPlayer() { return state.cast.some((m) => m.kind === "player"); }
function partnerCount() { return state.cast.reduce((n, m) => n + (m.kind === "player" ? 0 : 1), 0); }

// `unlisted` keeps a scene out of the catalog browse and the wheel; the library tab is its own
// opt-in surface, so library cards are always visible (the generated vanilla packs are all
// file-level unlisted:true — gating them here would blank the whole library).
function unlistedVisible(s) { return !!s && (s.library || !s.unlisted || state.filters.authorMode || state.allUnlisted); }

function ensureSelection() {
  // ACTIVE tab: the selection is whichever running-scene card was tapped (the brief mirrors
  // it), and a running scene may live in EITHER lane — never re-pick from the browse pool.
  if (state.mode === "active" && state.selectedId && sceneById(state.selectedId)) return;
  // Track the fully-filtered pool of the current browse mode so the brief never inspects a
  // scene no row shows. Re-pick to the top visible scene if the selection is filtered out.
  const list = activeList();
  const vis = list.filter(browseVisible);
  if (state.selectedId && vis.some((s) => s.id === state.selectedId)) return;
  if (vis.length) { applySelection(vis[0].id); return; }
  // Nothing matches the active filters — keep a valid selection so the brief isn't blank.
  if (!state.selectedId || !list.some((s) => s.id === state.selectedId)) {
    const fallback = list.find(unlistedVisible) || list[0];
    applySelection(fallback ? fallback.id : null);
  }
}

/* =========================================================================
   EVALUATION  (readiness gates derived from live registry data)
   ========================================================================= */
function evalScene(s) {
  const castCount = castTokens().length;
  const actorCount = s.actorCount || 0;
  const hasRoles = actorCount > 0;
  const rolesGate = hasRoles && castCount >= actorCount;
  const overCast = hasRoles && castCount > actorCount;
  // The anchor gate is exact when the keyed furniture's match set is known: an anchor-bound
  // scene needs THIS anchor to fit, not just any furniture keyed.
  const matchKnown = !!(state.furniture && state.anchorMatch && state.anchorMatch.token === state.furniture.token);
  const anchorFits = matchKnown ? state.anchorMatch.ids.has(s.id) : true;
  const anchorGate = s.requiresFurniture ? (!!state.furniture && anchorFits) : true;
  const seated = hasRoles ? Math.min(castCount, actorCount) : 0;
  const issues = [];
  const blockers = [];
  // A scene with no fillable roles is never READY — it can't be seated or launched.
  if (!hasRoles) blockers.push("scene defines no roles");
  else if (!rolesGate) { const n = actorCount - castCount; issues.push(`needs ${n} more actor${n === 1 ? "" : "s"}`); }
  if (overCast) { const n = castCount - actorCount; blockers.push(`remove ${n} crew member${n === 1 ? "" : "s"}`); }
  if (!anchorGate) {
    const a = anchorFull(s);
    issues.push(state.furniture ? `this furniture doesn't fit${a ? ` (needs ${a})` : ""}` : (a ? `needs ${a}` : "needs furniture"));
  }
  const gaps = issues.length + blockers.length;
  const reason = gaps === 0 ? "Ready with the current crew and furniture." : [...issues, ...blockers].map(sentenceCase).join(". ") + ".";
  return { castCount, actorCount, hasRoles, rolesGate, overCast, anchorGate, seated, issues, blockers, gaps, reason };
}

function needsText(s, ev) {
  if (!ev.rolesGate) { const n = ev.actorCount - ev.castCount; return `+${n} actor${n === 1 ? "" : "s"}`; }
  if (!ev.anchorGate) { const a = anchorShort(s); return a ? `needs ${a}` : (state.furniture ? "other furniture" : "needs furniture"); }
  if (ev.overCast) { const n = ev.castCount - ev.actorCount; return `-${n} crew`; }
  return "";
}

// What a furniture-bound scene anchors to. Short form = first name (+N when several,
// badge/meta space is tight); full form joins them all for titles and the brief.
function anchorShort(s) {
  const a = s.anchors || [];
  return a.length ? (a.length > 1 ? `${a[0]} +${a.length - 1}` : a[0]) : "";
}
function anchorFull(s) {
  const a = s.anchors || [];
  return a.length ? a.join(" / ") : "";
}

/* =========================================================================
   FILTER
   ========================================================================= */
function matchesSearch(s) {
  const f = state.filters;
  if (!unlistedVisible(s)) return false;
  if (!f.search) return true;
  const roleText = (s.roles || []).map((r) => `${r.name} ${r.gender}`).join(" ");
  let hay = `${s.title} ${s.id} ${(s.tags || []).join(" ")} ${roleText} ${s.sourceFile}`.toLowerCase();
  if (s.library && s.stageHay) hay += " " + s.stageHay;  // library search reaches individual animations
  return hay.includes(f.search);
}

/* ---- species (skeleton family) filter ------------------------------------
   Vanilla creature/alien clips live under their own skeleton, so a human animation
   can't play on a terrormorph (and vice versa) — the engine binds ~19/128 bones and
   the body spazzes. The browser therefore shows each cast member only the animations
   that target its skeleton. The picked actor's species rides in on osf.pick/scan; a
   scene's species is derived from its clips (UIBridge). allSpecies overrides. */
function castSpeciesSet() {
  const set = new Set();
  for (const m of state.cast) set.add(m.kind === "player" ? "human" : (m.species || "human"));
  return set;
}

// The cast contains a non-human skeleton — the filter is actively narrowing, so surface it.
function castHasCreature() {
  for (const m of state.cast) {
    const sp = m.kind === "player" ? "human" : (m.species || "human");
    if (sp && sp !== "human") return true;
  }
  return false;
}

function speciesLabel(sp) {
  if (!sp || sp === "human") return "Human";
  return sp.replace(/([a-z])([A-Z])/g, "$1 $2").replace(/^./, (c) => c.toUpperCase());
}

// Does scene s target a skeleton family the current cast has? allSpecies shows everything.
function speciesVisible(s) {
  if (state.allSpecies) return true;
  const set = castSpeciesSet();
  if (set.size === 0) return true;
  return set.has(s.species || "human");
}

// What the browse column shows for the current mode/filters (used by ensureSelection too).
function browseVisible(s) {
  if (!matchesSearch(s)) return false;
  if (!speciesVisible(s)) return false;
  if (state.mode === "scenes" && !state.browseAll) return evalScene(s).gaps === 0;
  return true;
}

// A one-line filter indicator + escape hatch, shown only when the cast narrows species.
function speciesFilterBarHTML() {
  if (!castHasCreature() && !state.allSpecies) return "";
  const label = state.allSpecies
    ? "ALL SPECIES"
    : [...castSpeciesSet()].map(speciesLabel).join(" + ").toUpperCase() + " ONLY";
  return `<div class="browse-note species"><span class="dot ${state.allSpecies ? "" : "go"}"></span>` +
    `<span class="lbl">${esc(label)}</span>` +
    `<button class="reveal inline ${state.allSpecies ? "on" : ""}" data-act="species-all">` +
    `${state.allSpecies ? "match cast" : "show all species"}</button></div>`;
}

/* =========================================================================
   RENDER
   ========================================================================= */
function renderAll() {
  // Wheel mode: the console/brief are hidden, so only the wheel needs (re)rendering — data
  // pushes that land while it's open (catalog refresh) route here too.
  if (state.wheel) { renderWheel(); return; }
  // Panels are rebuilt via innerHTML, which drops the focused control — capture where the
  // gamepad/keyboard focus is, rebuild, then restore it to the equivalent control (see NAV).
  const fk = navFocusKey(document.activeElement);
  ensureSelection();
  renderSlateTake();
  renderLivebar();
  renderRail();
  renderBrowse();
  renderBrief();
  navRestore(fk);
}

/* ---- slate: live take ----------------------------------------------------- */
// Compact header summary only — the dedicated ACTIVE tab (browse mode "active") is the
// management surface. A single running scene shows directly (title + its stop); several
// collapse to a count. Either chip opens the tab.
function renderSlateTake() {
  $("authorToggle").classList.toggle("on", state.filters.authorMode);
  const el = $("slateTake");
  const live = activeScenes();
  if (!live.length) {
    el.innerHTML = `<div class="take-chip"><span class="lbl">NO SCENE RUNNING</span><span class="mono">crew → furniture → launch</span></div>`;
    return;
  }
  if (live.length === 1) {
    const s = live[0];
    const cast = (s.cast || []).map((c) => c.name).join(" + ");
    el.innerHTML = `<div class="take-chip live"><span class="live-dot"></span>` +
      `<button class="take-body take-open" data-act="mode" data-mode="active" title="Manage in the ACTIVE tab"><span class="lbl">RUNNING · #${s.handle}${s.player ? " · YOU" : ""}</span><strong>${esc(sceneTitle(s.sceneId))}</strong>${cast ? `<span class="take-cast mono">${esc(cast)}</span>` : ""}</button>` +
      `<button class="stop-mini" data-act="stop-scene" data-handle="${s.handle}" title="Stop this scene">■ STOP</button></div>`;
    return;
  }
  const titles = live.map((s) => sceneTitle(s.sceneId)).join(" · ");
  el.innerHTML = `<div class="take-chip live"><span class="live-dot"></span>` +
    `<button class="take-body take-open" data-act="mode" data-mode="active" title="Manage in the ACTIVE tab"><span class="lbl">RUNNING · ${live.length} SCENES</span><strong>${esc(titles)}</strong><span class="take-cast mono">manage in ACTIVE ▸</span></button></div>`;
}

/* ---- live mode (minimized) ------------------------------------------------ */
// The console/brief hide (CSS via body.live-mode, same pattern as wheel-mode) and only the
// floating live bar stays: the whole screen becomes the world, so drag-to-orbit works
// everywhere. Entered on launch success or the slate minimize button; EXPAND/stop restores.
function setMinimized(min) {
  min = !!min;
  if (state.minimized === min) return;
  state.minimized = min;
  document.body.classList.toggle("live-mode", min);
  renderLivebar();
  if (min) {
    // The focused control just vanished with the console — move focus to the bar so
    // gamepad/keyboard users keep a live cursor (mouse users won't see the ring).
    const first = document.querySelector("#livebar [data-act]");
    if (first && document.body.classList.contains("nav-kb")) first.focus({ preventScroll: true });
  }
}

function renderLivebar() {
  const el = $("livebar");
  if (!state.minimized) { el.innerHTML = ""; return; }
  const running = !!state.lastHandle;
  const ls = sceneById(state.lastSceneId);
  const label = running
    ? `<span class="live-dot"></span><div class="take-body"><span class="lbl">RUNNING · #${state.lastHandle}</span><strong>${esc(ls ? ls.title : state.lastSceneId)}</strong></div>`
    : `<div class="take-body"><span class="lbl">STANDBY</span><strong>No scene running</strong></div>`;
  el.innerHTML = `<div class="take-chip ${running ? "live" : ""}">${label}` +
    (running ? `<button class="stop-mini" data-act="stop" title="Stop the running scene">■ STOP</button>` : "") +
    `<button class="expand-mini" data-act="expand" title="Bring the browser back"><svg class="chev-ico" width="10" height="6" viewBox="0 0 10 6" aria-hidden="true"><path d="M1 5l4-3.8L9 5"/></svg>BROWSER</button></div>` +
    `<div class="livebar-hint mono">DRAG ORBIT · WHEEL ZOOM</div>`;
}

/* ---- guided rail ---------------------------------------------------------- */
function renderRail() {
  $("rail").innerHTML = stepCastHTML() + stepAnchorHTML();
}

// Clickable step header; a collapsed step folds to this single line (note = the summary).
function stepHeadHTML(step, num, title, note, open) {
  return `<button class="step-head" data-act="step-toggle" data-step="${step}"><span class="step-num">${num}</span><span class="eb">${title}</span><span class="step-note">${note}</span><span class="chev">${open ? "▾" : "▸"}</span></button>`;
}

// Scan-row face: a neutral silhouette placeholder (the browser does not capture actor portraits).
function faceHTML() {
  return `<span class="near-face blank" aria-hidden="true"><svg viewBox="0 0 24 24"><circle cx="12" cy="9" r="4"/><path d="M4 22c0-4.4 3.6-7 8-7s8 2.6 8 7"/></svg></span>`;
}

function stepCastHTML() {
  const members = castMembers();
  const castCount = members.length;
  const open = state.stepOpen.cast;
  if (!open) {
    // Folded: who's on set, still readable at a glance.
    const summary = castCount === 0 ? "No cast" : members.map((m) => m.name).join(" + ");
    return `<div class="step closed">${stepHeadHTML("cast", 1, "CREW", esc(summary), false)}</div>`;
  }

  // CREW here is membership only (add / remove / re-add player). Order is DISPLAYED (A/B/C keys) but
  // SET in the brief's ROLES map for the selected scene — see roleMapHTML.
  const busy = busyTokens();
  const chips = members.map((m, i) => {
    const player = m.kind === "player";
    // Player and partners are both droppable; the player's × toggles it out (keeps the re-add ghost).
    const drop = player
      ? `<button class="chip-x" data-act="toggle-player" title="Remove player (NPC-only scene)">×</button>`
      : `<button class="chip-x" data-act="drop" data-i="${i}" title="Remove from crew">×</button>`;
    // Show the detected skeleton family for a non-human cast member so the species filter is legible.
    const sp = !player && m.species && m.species !== "human"
      ? `<span class="cast-species" title="Skeleton family — the library filters to its animations">${esc(speciesLabel(m.species))}</span>` : "";
    const live = busy.has(m.token)
      ? `<span class="cast-busy" title="Currently in a running scene — launching on them replaces it">LIVE</span>` : "";
    return `<span class="castline ${player ? "player" : ""}"><span class="cast-key">${String.fromCharCode(65 + i)}</span><span class="castline-name">${esc(m.name)}</span>${sp}${live}${drop}</span>`;
  }).join("");
  // When the player has been dropped, offer a ghost chip to put them back.
  const readd = hasPlayer() ? "" : `<button class="castline ghost" data-act="toggle-player" title="Add player back to the crew">＋ Player</button>`;

  const rows = state.nearbyActors.length
    ? state.nearbyActors.map((a) => {
        const added = state.cast.some((m) => m.token === a.token);
        const live = busy.has(a.token) ? `<span class="cast-busy" title="Currently in a running scene">LIVE</span>` : "";
        return `<button class="near-row ${added ? "active" : ""}" data-act="toggle-actor" data-token="${a.token}">${faceHTML(a)}<span class="near-name">${esc(a.name)}</span>${live}<span class="near-meta mono">${a.distance != null ? Math.max(1, Math.round(a.distance)) + "m" : ""}</span><span class="near-tag ${added ? "added" : ""}">${added ? "✓" : "ADD"}</span></button>`;
      }).join("")
    : `<div class="empty-mini"><span class="mono">Scan, or aim at someone before opening and PICK.</span></div>`;

  const fit = sceneCatalog().filter((s) => unlistedVisible(s) && (s.actorCount || 0) === castCount).length;
  const animNote = castCount === 1 ? (state.libraryReceived ? ` · animations ${animationList().length}` : " · + animations") : "";
  const foot = state.catalogReceived
    ? `<div class="step-foot"><span class="mono">${fit} scene${fit === 1 ? "" : "s"} fit ${castCount} actor${castCount === 1 ? "" : "s"}${animNote}</span></div>`
    : "";

  return `<div class="step">
    ${stepHeadHTML("cast", 1, "CREW", `${castCount} on deck`, true)}
    <div class="cast-stack">${chips}${readd}</div>
    ${castCount > 1 ? `<div class="cast-order-hint mono">A·B·C order sets roles — arrange in ROLES →</div>` : ""}
    <div class="step-sub"><span class="lbl">NEARBY</span><span class="step-tools"><button class="chip-btn" data-act="scan" data-kind="actor">SCAN</button><button class="chip-btn" data-act="pick" data-slot="actor">PICK</button></span></div>
    <div class="near-list">${rows}</div>
    ${foot}
  </div>`;
}

// Split the anchor's fitting scenes into custom (user-authored, catalog lane) vs vanilla
// (generated library packs). Custom leads; the vanilla library trails as a deprioritized note.
// Anything not in the catalog can only be a library-pack scene, so this holds even before the
// library lane has been fetched.
function anchorFitLabel() {
  const catIds = new Set(state.catalog.map((s) => s.id));
  let custom = 0;
  for (const id of state.anchorMatch.ids) if (catIds.has(id)) custom++;
  const lib = state.anchorMatch.ids.size - custom;
  const head = `${custom} custom scene${custom === 1 ? " keys" : "s key"} to this`;
  return lib ? `${head}<span class="lib-note"> · library ${lib}</span>` : head;
}

function anchorRow(an, keyed) {
  const active = keyed && keyed.token === an.token;
  // Custom (authored) scenes are the headline count; the vanilla-library remainder trails as a
  // muted, passive badge so it doesn't compete for attention. customCount may be null on an
  // older bridge — fall back to the total so the row still shows something useful.
  const total = an.sceneCount;
  const custom = an.customCount != null ? an.customCount : total;
  const lib = total != null && custom != null ? Math.max(0, total - custom) : 0;
  let unlocks = "";
  if (an.customCount != null) {
    unlocks = `<span class="near-badge ${custom ? "" : "empty"}" title="Custom scenes this furniture unlocks">${custom} fit</span>`;
    if (lib) unlocks += `<span class="near-badge lib" title="Vanilla library scenes this furniture unlocks">+${lib}</span>`;
  } else if (total != null) {
    unlocks = `<span class="near-badge" title="Scenes this furniture unlocks">${total} fit</span>`;
  }
  return `<button class="near-row ${active ? "active" : ""} ${an.marker ? "marker" : ""}" data-act="toggle-anchor" data-token="${an.token}"><span class="near-name">${esc(an.name)}</span>${unlocks}<span class="near-meta mono">${an.distance != null ? Math.max(1, Math.round(an.distance)) + "m" : ""}</span><span class="near-tag ${active ? "added" : ""}">${active ? "✓" : "USE"}</span></button>`;
}

function stepAnchorHTML() {
  const keyed = state.furniture;
  const matchKnown = !!(keyed && state.anchorMatch && state.anchorMatch.token === keyed.token);
  if (!state.stepOpen.anchor) {
    return `<div class="step closed">${stepHeadHTML("anchor", 2, "FURNITURE", keyed ? esc(keyed.name) : "none", false)}</div>`;
  }
  const slot = keyed
    ? `<div class="anchor-slot keyed"><span class="dot go"></span><span class="anchor-name">${esc(keyed.name)}</span><button class="chip-btn" data-act="clear-anchor">CLR</button></div>`
    : `<div class="anchor-slot"><span class="dot"></span><span class="anchor-name faint">none — free-space scenes only</span></div>`;

  // Visible furniture leads; invisible AI/idle markers (unnamed sandbox spots — still real
  // anchors, placed by level designers) trail in a collapsed group so they can't drown the list.
  const furniture = state.nearbyFurniture.filter((an) => !an.marker);
  const markers = state.nearbyFurniture.filter((an) => an.marker);
  let rows = furniture.length
    ? furniture.map((an) => anchorRow(an, keyed)).join("")
    : `<div class="empty-mini"><span class="mono">${markers.length ? "No visible furniture — try the AI markers below." : "Optional. Scan for usable furniture nearby."}</span></div>`;
  if (markers.length) {
    // Keep the group open while the keyed anchor lives inside it, so the ✓ row stays visible.
    const open = state.markersOpen || !!(keyed && markers.some((m) => m.token === keyed.token));
    rows += `<button class="reveal ${open ? "on" : ""}" data-act="markers-toggle" title="Invisible AI idle spots — usable like furniture, but you can't see them in the world">${open ? "▾" : "▸"} ${markers.length} AI marker${markers.length === 1 ? "" : "s"} (invisible)</button>`;
    if (open) rows += markers.map((an) => anchorRow(an, keyed)).join("");
  }

  const foot = keyed
    ? `<div class="step-foot"><span class="mono">${matchKnown ? anchorFitLabel() : "checking what fits…"}</span></div>`
    : "";

  return `<div class="step">
    ${stepHeadHTML("anchor", 2, "FURNITURE", "optional", true)}
    ${slot}
    <div class="step-sub"><span class="lbl">NEARBY</span><span class="step-tools"><button class="chip-btn" data-act="scan" data-kind="furniture">SCAN</button><button class="chip-btn" data-act="pick" data-slot="furniture">PICK</button></span></div>
    <div class="near-list">${rows}</div>
    ${foot}
  </div>`;
}

/* ---- browse ---------------------------------------------------------------- */
function renderBrowse() {
  const live = activeScenes();
  // The ACTIVE tab exists only while something runs — fall back when the last scene ends.
  if (state.mode === "active" && !live.length) state.mode = "scenes";
  const scenes = sceneCatalog();
  const emotes = emoteCatalog();
  const animCount = state.libraryReceived
    ? emotes.length + state.library.reduce((n, s) => n + (s.stages || []).length, 0)
    : `${emotes.length}+`;
  $("modeSwitch").innerHTML =
    `<button class="mode-btn ${state.mode === "scenes" ? "on" : ""}" data-act="mode" data-mode="scenes">SCENES · ${scenes.length}</button>` +
    `<button class="mode-btn ${state.mode === "library" ? "on" : ""}" data-act="mode" data-mode="library">ANIMATIONS · ${animCount}</button>` +
    (live.length
      ? `<button class="mode-btn live ${state.mode === "active" ? "on" : ""}" data-act="mode" data-mode="active" title="Running scenes"><span class="live-dot"></span>ACTIVE · ${live.length}</button>`
      : "");
  $("browseBody").innerHTML = state.mode === "active" ? activeTabHTML()
    : state.mode === "library" ? libraryBrowserHTML() : scenesBrowserHTML();
}

// Library animations that would play with the CURRENT crew (same readiness gate scenes use —
// role count, species, anchor). Used to route from an empty scenes lane to the library.
function libraryFitList() {
  return animationList().filter((s) => unlistedVisible(s) && speciesVisible(s) && evalScene(s).gaps === 0);
}

// The scenes lane has nothing playable for this crew. Installed scene packs are commonly all
// multi-actor, so a solo (player-only) crew looks empty here even though the vanilla animation
// library — a separate lane — has hundreds that play right now. Surface it instead of telling the
// user to "add crew". The library loads lazily; kick a one-shot fetch so the fit count fills in.
function emptyScenesRouteHTML() {
  if (!state.libraryReceived && libraryTries === 0) requestLibrary(false);
  const note = state.furniture || partnerCount()
    ? "No scene pack fits this exact crew + furniture."
    : "No solo scenes in your installed packs.";
  const n = state.libraryReceived ? libraryFitList().length : -1;
  const label = n > 0 ? `${n} animation${n === 1 ? "" : "s"} play with this crew ▸` : "OPEN ANIMATIONS ▸";
  return `<div class="bay-empty"><span class="mono">${esc(note)}</span>` +
    `<button class="chip-btn" data-act="mode" data-mode="library" style="margin-top:8px">${label}</button></div>`;
}

function scenesBrowserHTML() {
  if (!state.catalogReceived) return bayEmpty("Waiting for the catalog…");
  const scenes = sceneCatalog();
  // Fresh install with no scene packs: the vanilla library IS the out-of-box content — route there.
  if (scenes.length === 0) {
    return `<div class="bay-empty"><span class="mono">No authored scenes installed — emotes and the animation library are ready to play.</span><button class="chip-btn" data-act="mode" data-mode="library" style="margin-top:8px">OPEN ANIMATIONS ▸</button></div>`;
  }
  const searched = scenes.filter((s) => matchesSearch(s) && speciesVisible(s));
  const evald = searched.map((s) => ({ s, ev: evalScene(s) }));
  const playable = evald.filter((x) => x.ev.gaps === 0);
  const rest = evald.filter((x) => x.ev.gaps > 0);
  // With an anchor keyed, float the scenes that actually fit that furniture above the
  // free-space ones — so keying a chair surfaces the chair scenes first, not last.
  const anchorFit = (x) => !!(state.furniture && x.s.requiresFurniture && x.ev.anchorGate);
  const byRank = (a, b) => (anchorFit(b) - anchorFit(a)) || b.s.priority - a.s.priority || b.s.weight - a.s.weight || a.s.title.localeCompare(b.s.title);
  playable.sort(byRank); rest.sort(byRank);

  let html = speciesFilterBarHTML();
  html += `<div class="browse-note"><span class="dot go"></span><span class="lbl">PLAYABLE NOW · ${playable.length}</span></div>`;
  html += playable.length
    ? `<div class="row-list">${playable.map((x) => sceneRow(x.s, x.ev, true)).join("")}</div>`
    : emptyScenesRouteHTML();
  if (rest.length) {
    html += `<button class="reveal ${state.browseAll ? "on" : ""}" data-act="browse-all">${state.browseAll ? "▾" : "▸"} ${rest.length} more need a different crew or furniture</button>`;
    if (state.browseAll) html += `<div class="row-list dim">${rest.map((x) => sceneRow(x.s, x.ev, false)).join("")}</div>`;
  }
  return html;
}

function sceneRow(s, ev, playable) {
  const sel = s.id === state.selectedId;
  const n = s.actorCount || 1;
  const dur = fmtEst(s);
  const bits = [`${n} role${n === 1 ? "" : "s"}`];
  if (s.requiresFurniture) bits.push(`on ${anchorShort(s) || "furniture"}`);
  if (dur) bits.push(dur);
  const meta = state.filters.authorMode ? s.id : bits.join(" · ");
  const badge = playable ? `<span class="row-badge go">READY</span>` : `<span class="row-badge">${esc(needsText(s, ev))}</span>`;
  const pinmark = s.pinned > 0 ? `<span class="libx-pinmark" title="On the emote wheel">◆</span>` : "";
  return `<button class="libx-row ${sel ? "selected" : ""}" data-act="select-scene" data-id="${escAttr(s.id)}"><span class="libx-spine"></span><span class="libx-title">${esc(s.title)}</span>${pinmark}<span class="libx-meta mono">${esc(meta)}</span>${badge}</button>`;
}

/* ---- library browser (reference lane: compact folder-grouped rows) ------- */
function packKey(s) {
  const f = String(s.sourceFile || "").replace(/\\/g, "/").split("/").pop() || "";
  if (f) return f.replace(/\.osf\.json$/i, "");
  // The runtime payload carries no sourceFile — group by the id's leading pack family
  // instead ("vanilla/furniture/absorbpower" -> "vanilla-furniture"). Two segments max:
  // deeper ids (generated quest-scene sets) would otherwise explode into per-scene groups.
  // Two-segment ids keep both ("vanilla/photomode" -> "vanilla-photomode"), so the
  // top-level photomode/chargen sets land in their family group, not a bare "vanilla".
  const segs = String(s.id || "").split("/").filter(Boolean);
  return segs.slice(0, Math.max(1, Math.min(2, segs.length))).join("-") || "library";
}

function groupLabel(key) { return key.replace(/^vanilla-/i, "").replace(/[-_]+/g, " ").toUpperCase(); }

// null = anchor status irrelevant (free-space set, or no anchor keyed yet); true/false =
// this furniture-bound set does / doesn't fit the keyed anchor.
function fitsKeyedAnchor(s) {
  if (!s.requiresFurniture) return null;  // free-space: playable regardless of any anchor
  if (!state.furniture || !state.anchorMatch || state.anchorMatch.token !== state.furniture.token) return null;
  return state.anchorMatch.ids.has(s.id);
}

// Playability rank for the current setup: 0 = plays right now (fits the keyed furniture,
// or needs none while none is keyed), 1 = free-space set while furniture is keyed (still
// playable, but not what the keyed piece is for), 2 = furniture-bound with nothing
// (matching) keyed. Drives the library ordering so what fits the setup surfaces first.
function libRank(s) {
  if (!s.requiresFurniture) return state.furniture ? 1 : 0;
  return fitsKeyedAnchor(s) === true ? 0 : 2;
}

// The vanilla dump is mostly connective tissue: enter/exit/turn transitions and partial-body
// layers. A stage is CLEAN when it's a standalone whole — a pose or a loopable full clip.
// The library browses the clean tier by default; the full dump is one toggle away. Fit-focus
// (furniture keyed) bypasses the tier: the anchor match already curates, and e.g. dance
// flavor clips carry is_state=0 upstream (tagged 'transition') yet ARE the good content.
function stageClean(st) {
  const t = st.tags || [];
  return !t.includes("transition") && !t.includes("partial");
}
function cleanStages(s) { return (s.stages || []).filter(stageClean); }

// Clean-tier quality rank, best first: 0 = photomode POSE sets (the showpieces — hand-authored
// standing poses), 1 = other photomode (vehicle idles etc.), 2 = pose-dominated sets elsewhere
// (chargen), 3 = sets holding idle loops, 4 = the rest. Pose-dominance = MOST clean stages are
// 'pose'-tagged, so a lone 2-frame 'pose' clip in a grab-bag set can't promote it. Keeping
// photomode's pose member at 0 keeps the PHOTOMODE group on top (group rank = its best member)
// while ordering the real poses ahead of the vehicle idles inside it.
function setQuality(s) {
  const isPhoto = /(^|\/)photomode(\/|$)/.test(String(s.id));
  const cs = cleanStages(s);
  const poseMajority = cs.length > 0 && cs.filter((st) => (st.tags || []).includes("pose")).length > cs.length / 2;
  if (isPhoto && poseMajority) return 0;
  if (isPhoto) return 1;
  if (poseMajority) return 2;
  if (cs.some((st) => /idle/i.test(st.name || "") || (st.tags || []).some((t) => t.startsWith("idle")))) return 3;
  return 4;
}

function libraryGroups(pred, cleanTier) {
  const m = new Map();
  for (const s of state.library) {
    if (!matchesSearch(s)) continue;
    if (!speciesVisible(s)) continue;  // creature packs only appear for a matching creature cast
    if (pred && !pred(s)) continue;
    if (cleanTier && !cleanStages(s).length) continue;  // transition-only sets live in the full dump
    const key = packKey(s);
    if (!m.has(key)) m.set(key, []);
    m.get(key).push(s);
  }
  const quality = cleanTier ? setQuality : () => 0;
  for (const list of m.values()) list.sort((a, b) => libRank(a) - libRank(b) || quality(a) - quality(b) || a.title.localeCompare(b.title));
  // Groups by their MEAN member rank, so a mostly-anchored pack sinks when nothing fits
  // even if it holds a few free-space sets (those still sort first inside the group).
  const groupRank = (list) => list.reduce((a, s) => a + libRank(s), 0) / list.length;
  const groupQuality = (list) => Math.min(...list.map(quality));
  return [...m.entries()].sort((a, b) => groupRank(a[1]) - groupRank(b[1]) || groupQuality(a[1]) - groupQuality(b[1]) || a[0].localeCompare(b[0]));
}

function libraryBrowserHTML() {
  const emotes = emoteCatalog().filter((s) => matchesSearch(s) && speciesVisible(s));
  const emoteRows = emotes.map((s) => {
    const sel = s.id === state.selectedId;
    const dur = fmtEst(s);
    const wheel = isOnWheel(s) ? `<span class="libx-pinmark" title="On the emote wheel">◆</span>` : "";
    const meta = state.filters.authorMode ? s.id : ["emote", dur].filter(Boolean).join(" · ");
    return `<button class="libx-row emote ${sel ? "selected" : ""}" data-act="select-scene" data-id="${escAttr(s.id)}"><span class="libx-spine"></span><span class="libx-title">${esc(s.title)}</span>${wheel}<span class="libx-meta mono">${esc(meta)}</span></button>`;
  }).join("");
  const emoteGroup = emotes.length
    ? `<div class="libx-group emotes"><div class="libx-head static"><span class="emote-mark">✦</span><span class="libx-name">EMOTES</span><span class="libx-meta mono">${emotes.length} QUICK ACTION${emotes.length === 1 ? "" : "S"}</span></div><div class="libx-list">${emoteRows}</div></div>`
    : "";
  if (!state.libraryReceived) return speciesFilterBarHTML() + emoteGroup + bayEmpty("Loading the animation library…");
  if (!state.library.length) return speciesFilterBarHTML() + emoteGroup + (emoteGroup ? "" : bayEmpty("No animations installed."));

  // When furniture is keyed, focus the library on sets that fit it (this is the "filter the
  // furniture animations by furniture" behavior) — with a one-click escape to the full list.
  const matchKnown = !!(state.furniture && state.anchorMatch && state.anchorMatch.token === state.furniture.token);
  const fitFocus = matchKnown && !state.libShowAll;
  // Clean tier: the free-space browse leads with poses/loops; searching opts into everything
  // (stage names are in the search hay — a hit must not be invisible in the list).
  const cleanTier = !matchKnown && !state.libFull && !state.filters.search;
  const groups = libraryGroups(fitFocus ? (s) => state.anchorMatch.ids.has(s.id) : null, cleanTier);

  // Counts reflect the active species filter, so "N sets" matches what the list actually shows.
  const speciesLib = state.library.filter(speciesVisible);
  let banner = speciesFilterBarHTML();
  if (matchKnown) {
    // Count LIBRARY sets that fit (anchorMatch also covers scenes-lane scenes, which don't belong here).
    const fitCount = speciesLib.filter((s) => matchesSearch(s) && state.anchorMatch.ids.has(s.id)).length;
    banner += `<div class="browse-note"><span class="dot go"></span><span class="lbl">${esc(state.furniture.name)} · ${fitCount} SET${fitCount === 1 ? "" : "S"} FIT</span>` +
      `<button class="reveal inline ${state.libShowAll ? "on" : ""}" data-act="lib-showall">${state.libShowAll ? "show fitting only" : "show all"}</button></div>`;
  } else {
    const total = speciesLib.length;
    const clips = speciesLib.reduce((a, s) => a + (s.stages || []).length, 0);
    const cleanClips = speciesLib.reduce((a, s) => a + cleanStages(s).length, 0);
    const label = cleanTier ? `ANIMATION LIBRARY · ${cleanClips} POSES & LOOPS` : `ANIMATION LIBRARY · ${clips} CLIPS IN ${total} SETS`;
    const toggle = state.filters.search ? "" :
      `<button class="reveal inline ${state.libFull ? "on" : ""}" data-act="lib-full">${state.libFull ? "poses & loops only" : `full library · ${clips} clips`}</button>`;
    banner += `<div class="browse-note"><span class="dot"></span><span class="lbl">${label}</span>${toggle}</div>`;
  }

  if (!groups.length) {
    const empty = castHasCreature() && !state.allSpecies
      ? `No ${[...castSpeciesSet()].map(speciesLabel).join(" / ")} animations in the library. Show all species, or change the cast.`
      : (fitFocus ? "No library sets fit this furniture. Show all, or key a different piece." : "Nothing in the library matches the filter.");
    return banner + emoteGroup + (emoteGroup ? "" : bayEmpty(empty));
  }
  const searching = !!state.filters.search;
  const body = groups.map(([key, list]) => {
    // Auto-expand when searching or an anchor is keyed (so the ⚓ fit/no-fit marks are visible).
    const open = searching || matchKnown || state.libOpen.has(key);
    const stageTotal = list.reduce((a, s) => a + (cleanTier ? cleanStages(s) : (s.stages || [])).length, 0);
    const rows = open ? `<div class="libx-list">${list.map((s) => libxRow(s, cleanTier)).join("")}</div>` : "";
    return `<div class="libx-group">` +
      `<button class="libx-head" data-act="lib-group" data-key="${escAttr(key)}"><span class="chev">${open ? "▾" : "▸"}</span><span class="libx-name">${esc(groupLabel(key))}</span><span class="libx-meta mono">${list.length} set${list.length === 1 ? "" : "s"} · ${stageTotal} anim${stageTotal === 1 ? "" : "s"}</span></button>` +
      rows + `</div>`;
  }).join("");
  return banner + emoteGroup + body;
}

function libxRow(s, cleanTier) {
  const sel = s.id === state.selectedId;
  const n = (cleanTier ? cleanStages(s) : (s.stages || [])).length;
  const title = s.title.replace(/^Vanilla · /, "");
  const meta = state.filters.authorMode ? s.id : `${n} anim${n === 1 ? "" : "s"}`;
  // "FURN" marks furniture-bound sets; when furniture is keyed, tint by whether this set fits it.
  const fits = fitsKeyedAnchor(s);
  const anchorMark = s.requiresFurniture
    ? `<span class="libx-anchor ${fits === true ? "fit" : fits === false ? "nofit" : ""}" title="${escAttr(anchorFull(s) ? `Needs: ${anchorFull(s)}` : "Needs matching furniture")}">FURN</span>`
    : "";
  return `<button class="libx-row ${sel ? "selected" : ""} ${fits === false ? "dim" : ""}" data-act="select-scene" data-id="${escAttr(s.id)}"><span class="libx-spine"></span><span class="libx-title">${esc(title)}</span>${anchorMark}<span class="libx-meta mono">${esc(meta)}</span></button>`;
}

function bayEmpty(msg) { return `<div class="bay-empty"><span class="mono">${esc(msg)}</span></div>`; }

/* ---- ACTIVE tab (dedicated running-scenes manager) ------------------------ */
// One card per running scene: title, handle, YOU, current stage, full cast, per-scene
// stop. Clicking a card selects the scene so the brief shows its full details. NPC-only
// scenes persist across browser sessions — a reopen resurfaces them here.
function activeTabHTML() {
  const live = activeScenes();
  if (!live.length) return bayEmpty("No scenes running.");
  const stopAll = live.length > 1
    ? `<button class="reveal inline stop-all" data-act="stop-all" title="Stop every running scene">■ STOP ALL</button>` : "";
  return `<div class="browse-note"><span class="dot go"></span><span class="lbl">RUNNING · ${live.length} SCENE${live.length === 1 ? "" : "S"}</span>${stopAll}</div>` +
    `<div class="active-list">${live.map(activeCardHTML).join("")}</div>`;
}

function activeCardHTML(s) {
  const scene = sceneById(s.sceneId);
  const title = scene ? scene.title : (s.sceneId || "scene");
  const sel = state.selectedId === s.sceneId;
  const stages = scene ? (scene.stages || []) : [];
  // Current stage, when the scene has distinct browsable stages (stage index is 0-based).
  const stageLine = stages.length > 1 && s.stage >= 0 && s.stage < stages.length
    ? `<div class="active-stage mono">STAGE ${s.stage + 1}/${stages.length} · ${esc(stageLabel(scene, s.stage).toUpperCase())}</div>`
    : "";
  const cast = (s.cast || []).map((c) =>
    `<span class="active-actor ${c.player ? "player" : ""}">${esc(c.name)}${c.player ? " · YOU" : ""}</span>`).join("");
  const meta = scene && fmtEst(scene) ? ` · ${fmtEst(scene)}` : "";
  return `<div class="active-card ${sel ? "selected" : ""}">` +
    `<button class="active-main" data-act="select-scene" data-id="${escAttr(s.sceneId)}" title="Show this scene's brief">` +
    `<div class="active-headline"><span class="live-dot"></span><span class="active-title">${esc(title)}</span><span class="active-handle mono">#${s.handle}${esc(meta)}</span></div>` +
    stageLine +
    (cast ? `<div class="active-cast">${cast}</div>` : "") +
    `</button>` +
    `<button class="stop-mini" data-act="stop-scene" data-handle="${s.handle}" title="Stop this scene">■ STOP</button></div>`;
}

/* ---- brief (slim) --------------------------------------------------------- */
function renderBrief() {
  const brief = $("brief");
  const s = sceneById(state.selectedId);
  if (!s) { brief.innerHTML = `<div class="brief-empty"><span class="mono">Nothing selected.</span></div>`; return; }
  const ev = evalScene(s);
  const allMet = ev.gaps === 0;
  const emote = isEmote(s);

  const dur = fmtEst(s);
  const readyText = emote ? (allMet ? "EMOTE · READY TO PLAY" : "EMOTE · NEEDS ONE ACTOR") : (allMet ? "READY TO LAUNCH" : "NOT SEATABLE YET");
  const head = `<div class="brief-status ${allMet ? "" : "warn"}"><span class="dot"></span><p class="eb">${readyText}</p></div>
    <div class="brief-title">${esc(s.title)}${dur ? `<span class="card-dur">${esc(dur)}</span>` : ""}</div>
    ${state.filters.authorMode ? `<div class="mono wrap brief-src">${esc(s.id)} · ${esc(s.sourceFile || "live registry")}</div>` : ""}`;

  // One readable line instead of the gauge/req/seat instrument cluster.
  const anchorBit = s.requiresFurniture
    ? (state.furniture
        ? (ev.anchorGate ? `on ${state.furniture.name}` : `this furniture doesn't fit${anchorFull(s) ? ` (needs ${anchorFull(s)})` : ""}`)
        : (anchorFull(s) ? `needs ${anchorFull(s)}` : "needs furniture"))
    : "free-space";
  const summaryText = emote ? `quick action · self-terminating · ${anchorBit}` : `${ev.seated}/${ev.actorCount || "?"} crew · ${anchorBit}`;
  const summary = `<div class="brief-line ${allMet ? "" : "warn"}"><span class="mono">${esc(summaryText)}</span></div>`;

  const stages = s.stages || [];
  const canPlay = state.ready && allMet;
  // Library sets: poses/loops lead, the connective transition/partial clips fold behind a
  // count (mirrors the library's clean tier — the noise is reachable, just not in the way).
  const cleanList = s.library ? stages.filter(stageClean) : stages;
  const noise = s.library ? stages.filter((st) => !stageClean(st)) : [];
  const folded = cleanList.length > 0 && noise.length > 0 && !state.briefFullAnims;
  const shown = cleanList.length ? (folded ? cleanList : [...cleanList, ...noise]) : stages;
  const foldBtn = cleanList.length && noise.length
    ? `<button class="reveal anim-fold ${state.briefFullAnims ? "on" : ""}" data-act="brief-anims">${folded ? `+ ${noise.length} transition${noise.length === 1 ? "" : "s"} & layers ▸` : "poses & loops only"}</button>`
    : "";
  const animBox = stages.length
    ? `<div class="info-box"><div class="lbl">ANIMATIONS · ${shown.length}${folded ? ` OF ${stages.length}` : ""}</div><div class="anim-list">${shown.map((st) => {
        const label = st.name || (emote ? (stages.length === 1 ? s.title : `Part ${st.index + 1}`) : `Stage ${st.index}`);
        const tags = (st.tags || []).slice(0, 3).map((t) => `<span class="pill">${esc(t)}</span>`).join("");
        const loop = fmtDur(st.loopSec);
        const d = loop || fmtDur(st.estSec);
        const durHTML = d ? `<span class="anim-dur" title="${loop ? "Loop length" : "Stage time"}">${esc(d)}${st.openEnded ? "∞" : ""}</span>` : "";
        return `<div class="anim-row"><div class="anim-main"><span class="anim-name">${esc(label)}</span><div class="anim-tags">${tags}</div></div>${durHTML}<button class="anim-play" data-act="play-stage" data-stage="${st.index}" ${canPlay ? "" : "disabled"} title="Play this animation">▶</button></div>`;
      }).join("")}${foldBtn}</div></div>`
    : "";

  // START OVERRIDES: collapsed to one line by default — the footer stays tight and the Launch
  // button keeps the room. The folded row still reads at a glance: any non-default settings show
  // as a summary, so a tweaked override can't hide behind the fold.
  const o = state.opts;
  const camNames = { thirdperson_hold: "3rd person", scene_orbit: "orbit", freefly: "free fly", vanity_orbit: "vanity" };
  const tweaks = [];
  if (o.strip !== "-1") tweaks.push(`strip ${o.strip === "1" ? "on" : "off"}`);
  if (o.lock !== "-1") tweaks.push(`lock ${o.lock === "1" ? "on" : "off"}`);
  if (o.camera) tweaks.push(`cam ${camNames[o.camera] || o.camera}`);
  if (Number(o.speed) !== 1) tweaks.push(`${Number(o.speed).toFixed(1)}x`);
  const optsSummary = tweaks.length ? tweaks.join(" · ") : "defaults";
  const overrideGrid = state.optsOpen
    ? `<div class="override-grid"><label class="override"><span class="lbl">STRIP</span><select class="select" data-field="strip">${optionTags([["-1", "Inherit"], ["1", "On"], ["0", "Off"]], o.strip)}</select></label><label class="override"><span class="lbl">LOCK PLAYER</span><select class="select" data-field="lock">${optionTags([["-1", "Inherit"], ["1", "On"], ["0", "Off"]], o.lock)}</select></label><label class="override"><span class="lbl">CAMERA</span><select class="select" data-field="camera">${optionTags([["", "Inherit"], ["thirdperson_hold", "Third person"], ["scene_orbit", "Scene orbit"], ["freefly", "Free fly"], ["vanity_orbit", "Vanity orbit"]], o.camera)}</select></label><label class="override"><span class="lbl">SPEED <b id="speedVal">${Number(o.speed).toFixed(1)}x</b></span><input id="optSpeed" class="range" type="range" min="0.1" max="3" step="0.1" value="${escAttr(o.speed)}"></label></div>`
    : "";
  const overrides = `<div class="overrides ${state.optsOpen ? "open" : ""}"><button class="overrides-head" data-act="opts-toggle" title="${state.optsOpen ? "Collapse" : "Expand"} start overrides"><span class="chev">${state.optsOpen ? "▾" : "▸"}</span><span class="lbl">START OVERRIDES</span>${state.optsOpen ? "" : `<span class="overrides-sum mono ${tweaks.length ? "hot" : ""}">${esc(optsSummary)}</span>`}</button>${overrideGrid}</div>`;

  const authorBoxes = state.filters.authorMode
    ? `<div class="info-box hud"><div class="lbl">DIAGNOSTICS</div><div class="kv-list">${diagRows(s, ev).map(([k, v]) => `<div class="kv"><span class="k">${esc(k)}</span><span class="v">${esc(v)}</span></div>`).join("")}</div></div>`
    : "";

  const reason = !state.ready ? "Engine not connected." : (allMet ? "" : ev.reason);
  const launchBtn = canPlay
    ? `<button class="launch-btn go" data-act="launch">▶ ${emote ? "Play Emote" : s.library ? "Play Animation Set" : "Launch Scene"}</button>`
    : `<button class="launch-btn blocked" disabled>${esc(!state.ready ? "Engine Offline" : `Blocked · ${ev.gaps} gap${ev.gaps > 1 ? "s" : ""}`)}</button>`;
  const stopBtn = state.lastHandle ? `<button class="stop-btn" data-act="stop">■ Stop #${state.lastHandle}</button>` : "";
  const reasonHTML = reason ? `<div class="mono wrap" style="color:var(--text-faint);text-align:center">${esc(reason)}</div>` : "";
  const launchStack = `<div class="launch-stack">${reasonHTML}${launchBtn}${stopBtn}</div>`;

  const onWheel = s.wheelEligible && isOnWheel(s);
  const wheelIds = wheelPool().map((x) => x.id);
  const wheelIndex = wheelIds.indexOf(s.id);
  const orderControls = onWheel && state.wheelCustomized
    ? `<span class="wheel-order mono">${wheelIndex + 1}/${wheelIds.length}</span><button class="pin-btn compact" data-act="wheel-up" data-id="${escAttr(s.id)}" ${wheelIndex <= 0 ? "disabled" : ""} title="Move earlier on wheel">←</button><button class="pin-btn compact" data-act="wheel-down" data-id="${escAttr(s.id)}" ${wheelIndex < 0 || wheelIndex >= wheelIds.length - 1 ? "disabled" : ""} title="Move later on wheel">→</button>`
    : "";
  const reset = state.wheelCustomized ? `<button class="pin-btn reset" data-act="wheel-reset" title="Restore installed default emotes">RESET DEFAULTS</button>` : "";
  const pinRow = s.wheelEligible
    ? `<div class="brief-pin"><button class="pin-btn ${onWheel ? "on" : ""}" data-act="pin-toggle" data-id="${escAttr(s.id)}" title="${onWheel ? "Remove from the emote wheel" : "Add to the emote wheel"}">${onWheel ? "◆ ON WHEEL" : "◇ ADD TO WHEEL"}</button>${orderControls}${reset}</div>`
    : "";

  // Fixed header (status/title/cast) + fixed footer (overrides + launch/stop); only the animations
  // band between them scrolls. The ▶ Launch button and its overrides stay in view no matter how many
  // stages a scene has — no more scrolling past a long list to reach the button.
  const roleMap = roleMapHTML(s, ev);

  brief.innerHTML = head + summary + pinRow +
    `<div class="brief-scroll">${roleMap}${animBox}${authorBoxes}</div>` +
    `<div class="brief-foot">${emote && !state.filters.authorMode ? "" : overrides}${launchStack}</div>`;
}

// Role→actor map for the SELECTED scene: pairs each named role with the cast member currently
// bound to that slot (binding is by cast order). Reorderable here — same moveMember/drag/Alt+↑↓
// machinery as the crew list, keyed by member index, so the two panels mirror each other.
// Only shown when order actually matters (a multi-actor scene with someone to place).
function roleMapHTML(s, ev) {
  const members = castMembers();
  const actorCount = ev.actorCount || 0;
  if (actorCount < 2 || members.length === 0) return "";

  const roles = s.roles || [];
  const reorderable = members.length > 1;
  const slots = Math.max(actorCount, members.length);
  let rows = "";
  for (let i = 0; i < slots; i++) {
    const inScene = i < actorCount;
    const role = inScene ? (roles[i] || {}) : null;
    const roleName = inScene ? (role.name || `role ${i + 1}`) : "bench";
    const gender = role && role.gender && role.gender !== "any" ? `<span class="role-g">${esc(role.gender)}</span>` : "";
    const m = members[i];
    if (m) {
      const player = m.kind === "player";
      const grip = reorderable ? `<span class="drag-grip" aria-hidden="true">⋮⋮</span>` : "";
      rows += `<div class="role-row ${inScene ? "" : "bench"}"><span class="role-name">${esc(roleName)}${gender}</span><span class="role-arrow">→</span>` +
        `<span class="castline ${player ? "player" : ""}" draggable="${reorderable}" data-i="${i}" tabindex="0" title="${reorderable ? "▲/▼, drag, or Alt+↑/↓ to reassign the role" : ""}">${grip}<span class="castline-name">${esc(m.name)}</span>${reorderBtns(i, members.length)}</span></div>`;
    } else {
      // Under-filled role: add another actor from the CREW panel on the left.
      rows += `<div class="role-row unfilled"><span class="role-name">${esc(roleName)}${gender}</span><span class="role-arrow">→</span><span class="role-empty mono">add an actor →</span></div>`;
    }
  }
  const over = members.length > actorCount
    ? `<div class="role-note mono">${members.length - actorCount} extra beyond this scene's roles — trim in CREW</div>`
    : "";
  return `<div class="info-box"><div class="lbl">ROLES · ${actorCount}</div><div class="role-map">${rows}</div>${over}</div>`;
}

function diagRows(s, ev) {
  const rows = [["weight · priority", `${s.weight} · ${s.priority}`]];
  (s.roles || []).forEach((r, i) => rows.push([`role ${String.fromCharCode(65 + i)} filter`, `${r.gender || "any"} · ${ev.seated > i ? "pass" : "open"}`]));
  rows.push(["anchor", (s.requiresFurniture ? (anchorFull(s) || "required") : "free-space") + " · " + (ev.anchorGate ? "pass" : "fail")]);
  rows.push(["stages · shape", `${(s.stages || []).length} · ${(s.shape && s.shape.kind) || "linear"}`]);
  rows.push(["policies", `strip ${s.policy.stripActors} · lock ${s.policy.lockPlayer} · fade ${s.policy.fade} · cam ${s.policy.camera}`]);
  rows.push(["est duration", fmtEst(s) || "unmeasured"]);
  return rows;
}

function optionTags(pairs, val) {
  return pairs.map(([v, l]) => `<option value="${escAttr(v)}" ${String(v) === String(val) ? "selected" : ""}>${esc(l)}</option>`).join("");
}

/* ---- durations ----------------------------------------------------------- */
// "45s" under a minute, "2:30" over — instrument-panel terse. Sub-second clips read "1s", not "0s".
function fmtDur(sec) {
  if (sec == null || !Number.isFinite(sec) || sec < 0) return "";
  const s = Math.max(1, Math.round(sec));
  if (s < 60) return `${s}s`;
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}

// Scene runtime estimate: always "~" (holds assume two loops), "+" = a stage is
// unmeasured (read "at least"), trailing "∞" = holds until advanced.
function fmtEst(s) {
  const t = fmtDur(s.estSec);
  if (!t) return s.openEnded ? "∞" : "";
  return `~${t}${s.estPartial ? "+" : ""}${s.openEnded ? "∞" : ""}`;
}

/* =========================================================================
   LAUNCH
   ========================================================================= */
function doLaunch(stageIndex) {
  const s = sceneById(state.selectedId);
  if (!s) return;
  const opts = { strip: Number(state.opts.strip), lockPlayer: Number(state.opts.lock), camera: state.opts.camera, speed: Number(state.opts.speed) };
  // Optional: enter directly on one browsable animation (a stage of the sequence). 0 = whole scene.
  const stage = Number.isInteger(stageIndex) ? stageIndex : 0;
  if (stage > 0) opts.stage = stage;
  const payload = { sceneId: s.id, castTokens: castTokens(), opts };
  const roleNames = launchRoleNames(s);
  if (roleNames.length === castTokens().length) payload.roleNames = roleNames;
  if (s.requiresFurniture && state.furniture) payload.furnitureToken = state.furniture.token;
  const what = stage > 0 ? `${s.title} · ${stageLabel(s, stage)}` : s.title;
  notice("info", `Launching "${what}"…`);
  send("osf.animation.launch", payload);
}

function stageLabel(s, index) {
  const st = (s.stages || []).find((x) => x.index === index);
  return st && st.name ? st.name : `stage ${index}`;
}

function launchRoleNames(s) {
  if (!s.roles || s.roles.length !== castTokens().length) return [];
  const names = s.roles.map((r) => r.name || "");
  if (names.some((name) => !name || /^role \d+$/i.test(name))) return [];
  return names;
}

function isOnWheel(s) { return !!s && wheelPool().some((x) => x.id === s.id); }

// Stamp an explicit ordered loadout locally, then persist the complete list. This is
// intentionally whole-state: the first edit materializes every installed default before
// changing one entry, so adding/removing an emote never makes its neighbours disappear.
function setWheelIds(ids) {
  state.wheelCustomized = true;
  for (const s of state.catalog) s.pinned = 0;
  ids.slice(0, WHEEL_MAX).forEach((id, i) => {
    const s = state.catalog.find((x) => x.id === id);
    if (s) s.pinned = i + 1;
  });
  send("osf.animation.wheel.set", { sceneIds: ids.slice(0, WHEEL_MAX) });
}

function togglePin(id) {
  const s = state.catalog.find((x) => x.id === id);
  if (!s || !s.wheelEligible) return;
  const ids = wheelPool().slice(0, WHEEL_MAX).map((x) => x.id);
  const at = ids.indexOf(id);
  const adding = at < 0;
  if (adding) {
    if (ids.length >= WHEEL_MAX) {
      notice("err", `The emote wheel is full (${WHEEL_MAX}/${WHEEL_MAX}). Remove one before adding "${s.title}".`);
      return;
    }
    ids.push(id);
  } else {
    ids.splice(at, 1);
  }
  setWheelIds(ids);
  notice("info", adding ? `"${s.title}" added to the emote wheel.` : `"${s.title}" removed from the emote wheel.`);
  renderAll();
}

function moveWheelEntry(id, dir) {
  const ids = wheelPool().slice(0, WHEEL_MAX).map((x) => x.id);
  const from = ids.indexOf(id), to = from + dir;
  if (from < 0 || to < 0 || to >= ids.length) return;
  [ids[from], ids[to]] = [ids[to], ids[from]];
  setWheelIds(ids);
  notice("info", "Emote wheel order updated.");
  renderAll();
}

function resetWheel() {
  state.wheelCustomized = false;
  for (const s of state.catalog) s.pinned = 0;
  send("osf.animation.wheel.set", { reset: true });
  notice("info", "Emote wheel reset to installed defaults.");
  renderAll();
}

// Stop one scene by handle (the ACTIVE rows), or with no argument the last-launched one
// (the legacy stop buttons in the brief/live bar).
function doStop(handle) {
  const h = Number(handle) || state.lastHandle;
  if (!h) return;
  send("osf.animation.stop", { handle: h });
  notice("info", `Stopping handle ${h}…`);
  if (state.active) state.active = state.active.filter((s) => s.handle !== h);  // optimistic; the end push reconciles
  if (h === state.lastHandle) {
    state.lastHandle = 0;
    setMinimized(false);  // nothing to watch anymore — bring the console back
  }
  renderAll();
}

/* =========================================================================
   EMOTE WHEEL  (transient radial mode — EmoteWheel_Plan.md)
   Opened natively (OpenWheel hotkey verb) via an osf.mode {mode:"wheel",
   tagPrefix, target} push: the console/brief hide and a ring of solo scenes
   tagged under tagPrefix appears around the untouched world center. A pick
   launches on the player (-1) or the crosshair target captured at open time,
   then asks the host to close (osf.requestClose — the view can't hide
   itself). Cancel = Esc / right-click / hub click. Closing is host-driven:
   exitWheel() runs off the ui.visibility hide relay, so wheel mode can never
   leak into a normal browser open.
   ========================================================================= */
const WHEEL_MAX = 12;                    // visible and persisted capacity

// Count-adaptive slice-track ellipse: near-circular for the handful of emotes the
// immersion pack ships, widening toward the old 12-slice ellipse as the ring fills
// (labels crowd at 16:9 without the stretch). Always clears the 128px hub.
function wheelGeom(n) {
  const t = Math.max(0, Math.min(1, (n - 3) / 9));  // <=3 slices -> 0, 12 -> 1
  return { rx: Math.round(150 + 100 * t), ry: Math.round(140 + 50 * t) };  // 150x140 .. 250x190
}

function enterWheel(p) {
  const t = p && p.target;
  const target = t && typeof t.token === "number" ? { token: t.token, name: String(t.name || "Target") } : null;
  const tagPrefix = String((p && p.tagPrefix) || "player.emote.").toLowerCase();
  if (state.wheel) {  // idempotent re-send (the native side replays osf.mode on osf.opened)
    state.wheel.tagPrefix = tagPrefix;
    state.wheel.target = target;
  } else {
    state.wheel = { tagPrefix, target, focus: 0, error: "", launching: "" };
    state.mode = "wheel";
  }
  document.body.classList.add("wheel-mode");
  if (!state.catalogReceived) requestCatalog(false);  // open race: the catalog may not be in yet
  renderWheel();
}

function exitWheel() {
  if (!state.wheel) return;
  state.wheel = null;
  state.mode = "scenes";
  document.body.classList.remove("wheel-mode");
  $("wheel").innerHTML = "";
  renderAll();
}

// Effective wheel loadout. Before customization it is derived from installed defaults
// (or the requested tag prefix for a specialized wheel open). On the first browser edit
// that complete visible list is materialized into `pinned` order and remains explicit,
// including the intentionally-empty case.
function wheelPool() {
  const eligible = state.catalog.filter((s) => !s.unlisted && s.wheelEligible);
  const pins = eligible.filter((s) => s.pinned > 0).sort((a, b) => a.pinned - b.pinned);
  if (state.wheelCustomized) return pins.slice(0, WHEEL_MAX);
  const pre = state.wheel ? state.wheel.tagPrefix : "";
  return eligible
    .filter((s) => pre
      ? (s.tags || []).some((t) => t.toLowerCase().startsWith(pre))
      : s.wheelDefault)
    .sort((a, b) => b.priority - a.priority || b.weight - a.weight || a.title.localeCompare(b.title))
    .slice(0, WHEEL_MAX);
}

function renderWheel() {
  const root = $("wheel");
  const w = state.wheel;
  if (!w) { root.innerHTML = ""; return; }
  const pool = wheelPool();
  const slices = pool;
  // Stash what this render shows: wheelPick reads w.pool, so a catalog push landing
  // between render and click can't shift indices under an in-flight pick.
  w.pool = slices;
  if (w.focus >= slices.length) w.focus = Math.max(0, slices.length - 1);
  const { rx, ry } = wheelGeom(slices.length);

  let body;
  if (!state.catalogReceived) {
    body = `<div class="wheel-empty"><span class="mono">Loading emotes…</span><button class="chip-btn" data-act="wheel-cancel">CLOSE</button></div>`;
  } else if (!slices.length) {
    body = `<div class="wheel-empty"><span class="mono">${state.wheelCustomized ? "Your emote wheel is empty. Reset it from the Animation Browser to restore installed defaults." : `No wheel-ready emotes carry a ${esc(w.tagPrefix)}* tag.`}</span><button class="chip-btn" data-act="wheel-cancel">CLOSE</button></div>`;
  } else {
    // Slices sit on the count-adaptive ellipse (wheelGeom), clockwise from the top.
    // Positions are inline — no CSS trig needed.
    const items = slices.map((s, i) => {
      const rad = ((-90 + (360 / slices.length) * i) * Math.PI) / 180;
      const x = Math.round(Math.cos(rad) * rx);
      const y = Math.round(Math.sin(rad) * ry);
      return `<button class="wheel-slice ${i === w.focus ? "focused" : ""}" data-act="wheel-pick" data-i="${i}" style="transform:translate(-50%,-50%) translate(${x}px,${y}px)" title="${escAttr(s.title)}">${w.launching === s.id ? "▶ " : ""}${esc(s.title)}</button>`;
    }).join("");
    const status = w.error
      ? `<span class="wheel-hub-status err">${esc(w.error)}</span>`
      : `<span class="wheel-hub-status">${w.launching ? "launching…" : "click to close"}</span>`;
    body = `<div class="wheel-dial"></div>${items}` +
      `<button class="wheel-hub" data-act="wheel-cancel" title="Close (Esc)"><span class="wheel-hub-who">${esc(w.target ? `→ ${w.target.name}` : "You")}</span>${status}</button>`;
  }
  root.innerHTML = `<div class="wheel-ring" style="--wrx:${rx}px;--wry:${ry}px">${body}</div>` +
    `<div class="wheel-caption mono">EMOTE WHEEL · ←→ SELECT · ENTER PLAY · ESC CLOSE</div>`;
  const f = root.querySelector(".wheel-slice.focused");
  if (f) f.focus({ preventScroll: true });
}

// Move the focused slice without a rebuild (arrow steps and mouse hover share this).
function wheelSetFocus(i) {
  const w = state.wheel;
  if (!w) return;
  const slices = [...document.querySelectorAll("#wheel .wheel-slice")];
  if (!slices.length) return;
  w.focus = ((i % slices.length) + slices.length) % slices.length;
  slices.forEach((el, k) => el.classList.toggle("focused", k === w.focus));
  slices[w.focus].focus({ preventScroll: true });
}

function wheelPick(i) {
  const w = state.wheel;
  if (!w || w.launching) return;  // one launch in flight at a time
  const s = (w.pool || [])[i];    // the rendered slices, not a fresh re-pool (see renderWheel)
  if (!s) return;
  w.error = "";
  w.launching = s.id;
  // No opts overrides — emote scenes carry their own camera/strip defaults.
  send("osf.animation.launch", { sceneId: s.id, castTokens: [w.target ? w.target.token : PLAYER_TOKEN] });
  renderWheel();
}

function wheelCancel() {
  send("osf.animation.requestClose");  // host hides the view; exitWheel lands via the ui.visibility relay
}

// Wheel-mode keys (routed from onNavKey before the spatial-nav machinery):
// arrows step the ring, Enter/Space picks, Escape cancels. Escape delivery
// in-game is unverified — the hub click and right-click cover that gap.
function onWheelKey(e) {
  const w = state.wheel;
  if (e.key === "Escape") { e.preventDefault(); wheelCancel(); return; }
  const dir = { ArrowUp: -1, ArrowLeft: -1, ArrowDown: 1, ArrowRight: 1 }[e.key];
  if (dir) {
    document.body.classList.add("nav-kb");
    e.preventDefault();
    wheelSetFocus(w.focus + dir);
    return;
  }
  if (e.key === "Enter" || e.key === " " || e.key === "Spacebar") {
    e.preventDefault();
    wheelPick(w.focus);
  }
}

/* =========================================================================
   UTIL
   ========================================================================= */
function sentenceCase(str) { const s = String(str || ""); return s ? s.charAt(0).toUpperCase() + s.slice(1) : s; }
function esc(s) { return String(s == null ? "" : s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c])); }
function escAttr(s) { return esc(s).replace(/`/g, "&#96;"); }
function setLamp(stateName) { $("lamp").dataset.state = stateName; }

let noticeTimer = null;
function notice(kind, text) {
  const el = $("notice");
  el.className = `notice ${kind}`;
  el.textContent = text;
  clearTimeout(noticeTimer);
  if (kind !== "err") noticeTimer = setTimeout(() => { el.className = "notice"; el.textContent = ""; }, 6000);
}

/* =========================================================================
   WIRING
   ========================================================================= */
function onClick(e) {
  const el = e.target.closest("[data-act]");
  if (!el) return;
  switch (el.dataset.act) {
    case "select-scene": selectScene(el.dataset.id); break;
    case "mode": setMode(el.dataset.mode === "library" || el.dataset.mode === "active" ? el.dataset.mode : "scenes"); break;
    case "lib-group": {
      const k = el.dataset.key;
      if (state.libOpen.has(k)) state.libOpen.delete(k);
      else state.libOpen.add(k);
      renderAll();
      break;
    }
    case "browse-all": state.browseAll = !state.browseAll; renderAll(); break;
    case "species-all": state.allSpecies = !state.allSpecies; renderAll(); break;
    case "markers-toggle": state.markersOpen = !state.markersOpen; renderAll(); break;
    case "step-toggle": {
      const k = el.dataset.step === "anchor" ? "anchor" : "cast";
      state.stepOpen[k] = !state.stepOpen[k];
      renderAll();
      break;
    }
    case "lib-showall": state.libShowAll = !state.libShowAll; renderAll(); break;
    case "lib-full": state.libFull = !state.libFull; renderAll(); break;
    case "brief-anims": state.briefFullAnims = !state.briefFullAnims; renderAll(); break;
    case "opts-toggle": state.optsOpen = !state.optsOpen; renderAll(); break;
    case "toggle-actor": toggleActor(Number(el.dataset.token)); break;
    case "toggle-anchor": toggleAnchor(Number(el.dataset.token)); break;
    case "scan": scanNearby(el.dataset.kind); break;
    case "pick": send("osf.animation.pickCrosshair", { slot: el.dataset.slot }); break;
    case "clear-anchor": clearAnchor(); renderAll(); break;
    case "drop": removeMember(Number(el.dataset.i)); break;
    case "toggle-player": togglePlayer(); break;
    case "move-up": nudgeMember(Number(el.dataset.i), -1, el.closest(".role-map") ? ".role-map" : ".cast-stack", "move-up"); break;
    case "move-down": nudgeMember(Number(el.dataset.i), 1, el.closest(".role-map") ? ".role-map" : ".cast-stack", "move-down"); break;
    case "launch": doLaunch(); break;
    case "stop": doStop(); break;
    case "stop-scene": doStop(Number(el.dataset.handle)); break;
    case "stop-all": for (const s of activeScenes().slice()) doStop(s.handle); break;
    case "pin-toggle": togglePin(el.dataset.id); break;
    case "wheel-up": moveWheelEntry(el.dataset.id, -1); break;
    case "wheel-down": moveWheelEntry(el.dataset.id, 1); break;
    case "wheel-reset": resetWheel(); break;
    case "minimize": setMinimized(true); break;
    case "expand": setMinimized(false); renderAll(); break;
    case "play-stage": doLaunch(Number(el.dataset.stage)); break;
    case "wheel-pick": wheelPick(Number(el.dataset.i)); break;
    case "wheel-cancel": wheelCancel(); break;
    default: break;
  }
}

function onChange(e) {
  const f = e.target.dataset && e.target.dataset.field;
  if (!f) return;
  if (f === "strip") state.opts.strip = e.target.value;
  else if (f === "lock") state.opts.lock = e.target.value;
  else if (f === "camera") state.opts.camera = e.target.value;
}

function onInput(e) {
  if (e.target.id !== "optSpeed") return;
  state.opts.speed = e.target.value;
  const sv = $("speedVal");
  if (sv) sv.textContent = `${Number(e.target.value).toFixed(1)}x`;
}

/* =========================================================================
   CAST REORDER  (drag-and-drop, plus Alt+↑/↓ for keyboard/gamepad)
   The role/slot each actor plays is bound by cast order, so this is the
   control for "who plays which role". Chips carry data-i = member index.
   ========================================================================= */
let dragFrom = -1;

function castLineAt(el) {
  const line = el && el.closest ? el.closest(".castline[draggable='true']") : null;
  return line && !line.classList.contains("ghost") ? line : null;
}

function clearDropHints() {
  document.querySelectorAll(".castline.drop-before, .castline.drop-after")
    .forEach((el) => el.classList.remove("drop-before", "drop-after"));
}

function endDrag() {
  dragFrom = -1;
  clearDropHints();
  document.querySelectorAll(".castline.dragging").forEach((el) => el.classList.remove("dragging"));
}

function onDragStart(e) {
  const line = castLineAt(e.target);
  if (!line) return;
  dragFrom = Number(line.dataset.i);
  e.dataTransfer.effectAllowed = "move";
  try { e.dataTransfer.setData("text/plain", String(dragFrom)); } catch (_) { /* some hosts disallow setData */ }
  line.classList.add("dragging");
}

function onDragOver(e) {
  if (dragFrom < 0) return;
  const line = castLineAt(e.target);
  if (!line) return;
  e.preventDefault();  // permit the drop
  e.dataTransfer.dropEffect = "move";
  const r = line.getBoundingClientRect();
  const after = (e.clientY - r.top) > r.height / 2;
  clearDropHints();
  if (Number(line.dataset.i) !== dragFrom) line.classList.add(after ? "drop-after" : "drop-before");
}

function onDrop(e) {
  if (dragFrom < 0) { endDrag(); return; }
  const line = castLineAt(e.target);
  if (!line) { endDrag(); return; }
  e.preventDefault();
  const to = Number(line.dataset.i);
  const r = line.getBoundingClientRect();
  const after = (e.clientY - r.top) > r.height / 2;
  const from = dragFrom;
  endDrag();
  if (moveMember(from, to, after)) { notice("info", "Cast order updated."); renderAll(); }
}

// Alt+↑/↓ nudges the focused chip (the nav handler ignores Alt chords, so no conflict).
function onReorderKey(e) {
  if (!e.altKey || (e.key !== "ArrowUp" && e.key !== "ArrowDown")) return;
  const line = castLineAt(document.activeElement);
  if (!line) return;
  e.preventDefault();
  const scopeSel = line.closest(".role-map") ? ".role-map" : ".cast-stack";
  nudgeMember(Number(line.dataset.i), e.key === "ArrowDown" ? 1 : -1, scopeSel);
}

/* =========================================================================
   GAMEPAD / KEYBOARD NAVIGATION
   OSF UI maps the controller D-pad/stick to arrow keys and A/Start to Enter,
   but the page had no keyboard-navigable focus model, so those keystrokes
   landed on nothing. This adds directional (spatial) focus movement over the
   console's controls, Enter-to-activate, per-control Left/Right semantics
   (slider nudge, dropdown cycle, search caret), and a focus ring that shows
   only while navigating by pad/key (body.nav-kb). Focus is kept stable across
   the panel re-renders renderAll() drives (see navFocusKey / navRestore).
   ========================================================================= */
const NAV_SEL = 'button, a[href], select, input, [tabindex]';

function navItems() {
  return [...document.querySelectorAll(NAV_SEL)].filter(navVisible);
}

// Focusable = enabled, in the DOM, and actually on screen. .brief is absolutely
// positioned so a plain offsetParent test is unreliable — test the rect directly.
function navVisible(el) {
  if (el.disabled || el.hidden) return false;
  if (el.getAttribute("tabindex") === "-1") return false;
  if (!el.getClientRects().length) return false;
  const b = el.getBoundingClientRect();
  if (b.width < 1 || b.height < 1) return false;
  return b.bottom > 0 && b.right > 0 && b.top < innerHeight && b.left < innerWidth;
}

function navCenter(el) {
  const b = el.getBoundingClientRect();
  return { x: b.left + b.width / 2, y: b.top + b.height / 2 };
}

// Best focusable in a compass direction: minimise travel along the axis plus a
// heavy penalty for cross-axis drift, so a list steps row-by-row and Left/Right
// hops cleanly between the rail, browse and brief columns.
function navMove(dir) {
  const items = navItems();
  if (!items.length) return;
  const cur = document.activeElement;
  if (!cur || cur === document.body || !items.includes(cur)) {
    // First move with nothing focused: drop into the scene list rather than the top toolbar.
    const entry = document.querySelector(".libx-row.selected") || document.querySelector(".libx-row");
    navFocus(entry && navVisible(entry) ? entry : items[0]);
    return;
  }
  const c = navCenter(cur);
  let best = null, bestScore = Infinity;
  for (const el of items) {
    if (el === cur) continue;
    const t = navCenter(el);
    const dx = t.x - c.x, dy = t.y - c.y;
    let along, cross;
    if (dir === "up") { if (dy > -1) continue; along = -dy; cross = Math.abs(dx); }
    else if (dir === "down") { if (dy < 1) continue; along = dy; cross = Math.abs(dx); }
    else if (dir === "left") { if (dx > -1) continue; along = -dx; cross = Math.abs(dy); }
    else { if (dx < 1) continue; along = dx; cross = Math.abs(dy); }
    const score = along + cross * 2.2;
    if (score < bestScore) { bestScore = score; best = el; }
  }
  if (best) navFocus(best);
}

function navFocus(el) {
  if (!el) return;
  el.focus({ preventScroll: true });
  el.scrollIntoView({ block: "nearest", inline: "nearest" });
}

// A gamepad can't drag a slider or open a native dropdown — Left/Right nudges/cycles instead.
function navNudgeRange(el, dir) {
  const step = Number(el.step) || 0.1, min = Number(el.min), max = Number(el.max);
  let v = Number(el.value) + (dir === "right" ? step : -step);
  v = Math.min(max, Math.max(min, Math.round(v / step) * step));
  el.value = String(v);
  el.dispatchEvent(new Event("input", { bubbles: true }));
}

function navCycleSelect(el, dir) {
  const n = el.options.length;
  if (!n) return;
  el.selectedIndex = (el.selectedIndex + (dir === "left" ? -1 : 1) + n) % n;
  el.dispatchEvent(new Event("change", { bubbles: true }));
}

function isTextEntry(el) {
  return !!el && el.tagName === "INPUT" && /^(?:text|search|url|email|tel|number|password|)$/.test(el.type);
}

function onNavKey(e) {
  if (e.altKey || e.ctrlKey || e.metaKey) return;  // leave real-keyboard chords alone
  if (state.wheel) { onWheelKey(e); return; }      // wheel mode owns the keys (ring step, pick, cancel)
  const el = document.activeElement;
  const dir = { ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" }[e.key];

  if (dir) {
    document.body.classList.add("nav-kb");
    if (dir === "left" || dir === "right") {
      if (el && el.tagName === "INPUT" && el.type === "range") { e.preventDefault(); navNudgeRange(el, dir); return; }
      if (el && el.tagName === "SELECT") { e.preventDefault(); navCycleSelect(el, dir); return; }
      if (isTextEntry(el)) return;  // let the caret move inside the search box
    }
    e.preventDefault();  // don't also scroll the container
    navMove(dir);
    return;
  }

  if (e.key === "Enter" || e.key === " " || e.key === "Spacebar") {
    if (!el || el === document.body || isTextEntry(el)) return;
    document.body.classList.add("nav-kb");
    if (el.tagName === "SELECT") { e.preventDefault(); navCycleSelect(el, "right"); return; }
    if (el.tagName === "INPUT" && el.type === "range") return;
    e.preventDefault();
    el.click();  // route through the existing delegated click / addEventListener handlers
    return;
  }

  if (e.key === "Tab") document.body.classList.add("nav-kb");
}

// Re-find the logically-equivalent control after a renderAll() rebuild: by id when it has
// one, else by its data-act plus discriminators. Falls back to the nearest control by
// geometry when the exact control is gone (a step folded, a row filtered out).
function navFocusKey(el) {
  if (!el || el === document.body) return null;
  if (el.id) return { sel: `#${cssEsc(el.id)}` };
  const act = el.dataset && el.dataset.act;
  if (!act) return null;
  const parts = [`[data-act="${attrEsc(act)}"]`];
  for (const k of ["id", "token", "step", "mode", "kind", "slot", "key", "stage", "field", "i"]) {
    const v = el.dataset[k];
    if (v != null) parts.push(`[data-${k}="${attrEsc(v)}"]`);
  }
  const c = navCenter(el);
  return { sel: parts.join(""), x: c.x, y: c.y };
}

function navRestore(fk) {
  if (!fk) return;
  let el = null;
  try { el = document.querySelector(fk.sel); } catch { el = null; }
  if (el) {
    if (document.activeElement === el) return;   // static control (e.g. #search) never lost focus
    if (navVisible(el)) { el.focus({ preventScroll: true }); return; }
  }
  if (fk.x == null) return;
  let best = null, bestD = Infinity;
  for (const it of navItems()) {
    const c = navCenter(it);
    const d = (c.x - fk.x) ** 2 + (c.y - fk.y) ** 2;
    if (d < bestD) { bestD = d; best = it; }
  }
  if (best) best.focus({ preventScroll: true });
}

function cssEsc(s) { return (window.CSS && CSS.escape) ? CSS.escape(s) : String(s).replace(/([^\w-])/g, "\\$1"); }
function attrEsc(s) { return String(s).replace(/(["\\])/g, "\\$1"); }

function initNav() {
  document.addEventListener("keydown", onNavKey);
  // A real pointer means the mouse is driving: drop the focus ring so it isn't shown to
  // mouse users. (A gamepad arrives as key events, never as mousemove, so the ring stays.)
  const mouseMode = () => document.body.classList.remove("nav-kb");
  document.addEventListener("mousedown", mouseMode);
  document.addEventListener("mousemove", mouseMode);
}

function init() {
  // Static browse skeleton: the search input must survive re-renders (focus + caret),
  // so only #modeSwitch and #browseBody are re-rendered.
  $("browse").innerHTML = `<div class="browse-head"><div class="mode-switch" id="modeSwitch"></div><div class="search-field grow"><input id="search" type="text" placeholder="⌕ search scenes · animations · tags" autocomplete="off" spellcheck="false"></div></div><div id="browseBody" class="browse-body"></div>`;

  document.addEventListener("click", onClick);
  document.addEventListener("change", onChange);
  document.addEventListener("input", onInput);
  // Cast reorder: HTML5 drag for mouse, Alt+↑/↓ for keyboard/gamepad.
  document.addEventListener("dragstart", onDragStart);
  document.addEventListener("dragover", onDragOver);
  document.addEventListener("drop", onDrop);
  document.addEventListener("dragend", endDrag);
  document.addEventListener("keydown", onReorderKey);
  // Gamepad/keyboard directional focus. The OSF UI runtime's default gamepad
  // mapping (protocol 1.0) feeds this: D-pad/left stick → arrow keys, A → Enter,
  // B → close, right stick → scroll. No raw ui.gamepad handling needed here.
  initNav();

  // Emote wheel: hover focuses a slice; right-click anywhere cancels (Escape's in-game
  // delivery is unverified, so the wheel never depends on it alone).
  document.addEventListener("mouseover", (e) => {
    const slice = state.wheel && e.target instanceof Element && e.target.closest(".wheel-slice");
    if (slice) wheelSetFocus(Number(slice.dataset.i));
  });
  document.addEventListener("contextmenu", (e) => {
    if (state.wheel) { e.preventDefault(); wheelCancel(); }
  });

  // Orbit drag (see the ORBIT DRAG block): LMB on the world area = steer, wheel there = zoom.
  document.addEventListener("mousedown", (e) => {
    if (e.button === 0 && orbitWorldTarget(e)) {
      orbit.dragging = true;
      orbit.x = e.clientX;
      orbit.y = e.clientY;
      e.preventDefault();  // no text selection while steering
    }
  });
  document.addEventListener("mousemove", (e) => {
    if (!orbit.dragging) return;
    orbit.dx += e.clientX - orbit.x;
    orbit.dy += e.clientY - orbit.y;
    orbit.x = e.clientX;
    orbit.y = e.clientY;
    orbitQueue();
  });
  document.addEventListener("mouseup", (e) => { if (e.button === 0) orbit.dragging = false; });
  document.addEventListener("wheel", (e) => {
    if (e.deltaY && orbitWorldTarget(e)) {
      orbit.wheel += e.deltaY < 0 ? 1 : -1;  // + = wheel up = zoom in (matches the native axis)
      orbitQueue();
    }
  }, { passive: true });
  $("refresh").addEventListener("click", () => {
    notice("info", "Refreshing catalog…");
    requestCatalog(true);
    if (state.libraryReceived || state.mode === "library") requestLibrary(true);
  });
  $("authorToggle").addEventListener("click", () => { state.filters.authorMode = !state.filters.authorMode; renderAll(); });
  $("search").addEventListener("input", (e) => { state.filters.search = e.target.value.trim().toLowerCase(); renderAll(); });

  renderAll();

  if (bridgeAvailable()) {
    setLamp("wait");
    $("statusText").textContent = "waiting for runtime…";
    requestCatalog(true);
  } else {
    setLamp("ok");
    state.ready = true;
    state.nearbyActors = MOCK_ACTORS.slice();
    state.nearbyFurniture = MOCK_ANCHORS.slice();
    // Prefer the committed snapshot fixtures in live/ (real catalog data);
    // fall back to the mock catalog when absent (e.g. a file:// page).
    fetchLiveSnapshot("catalog").then((live) => {
      $("statusText").textContent = live ? "standalone · live snapshot" : "standalone mock";
      handleCatalog(applyMockPins(live || MOCK_CATALOG));
      notice("info", live
        ? "Standalone mode. Snapshot catalog (live/catalog.json); pick/scan/launch are stubbed. W = emote wheel (Shift+W: no target) · B = backdrop."
        : "Standalone mode. Mock catalog, native calls are stubbed. W = emote wheel (Shift+W: no target) · B = backdrop.");
      // ?wheel boots straight into wheel mode (?wheel=solo: no target), so a plain
      // reload while iterating on the wheel doesn't need a W keypress every time.
      const q = new URLSearchParams(location.search);
      if (q.has("wheel")) window.mockOpenWheel(q.get("wheel") !== "solo");
    });
    // Emote-wheel dev: W opens the wheel with a mock crosshair target, Shift+W player-only
    // (also window.mockOpenWheel(withTarget) from the console). Feeds the same osf.mode
    // path the native OpenWheel uses. The wheel-mode debug strip (top-left) drives the
    // rest of the states — see WHEEL DEBUG below.
    initWheelDebug();
    initDevBackdrop();
    window.mockOpenWheel = (withTarget = true) => {
      wheelDbg.target = withTarget;
      renderWheelDbg();
      onNativeMessage(JSON.stringify({
        type: "osf.animation.mode",
        payload: { mode: "wheel", tagPrefix: "player.emote.", target: withTarget ? { token: 601, name: "Sarah Morgan" } : null },
      }));
    };
    document.addEventListener("keydown", (e) => {
      if (isTextEntry(document.activeElement)) return;
      if (e.key === "w") window.mockOpenWheel(true);
      else if (e.key === "W") window.mockOpenWheel(false);
    });
  }
}

/* =========================================================================
   MOCK (standalone dev — exercises normalizeScene / the bridge stubs)
   ========================================================================= */
// Solo free-space emotes for the wheel (player.emote.* tags) — 14 of them, so standalone
// exercises the hard 12-entry capacity. Facepalm mock-fails its launch (the error path).
const MOCK_EMOTES = ["Wave", "Cheer", "Clap", "Point", "Salute", "Shrug", "Facepalm", "Flex", "Dance", "Bow", "Thumbs Up", "Warm Hands", "Sit Ground", "Whistle"].map((name, i) => {
  const slug = name.toLowerCase().replace(/\s+/g, "");
  return { id: `emote.${slug}`, title: name, tags: [`player.emote.${slug}`, "emote"], actorCount: 1, genders: ["any"], requiresFurniture: false, estSec: 4 + (i % 5), priority: 0, weight: 1, sourceFile: "Data/OSF/Emotes/immersion.osf.json" };
});
// Standalone explicit loadout state, applied on top of either catalog source.
let MOCK_WHEEL_CUSTOMIZED = false;
let MOCK_PINS = [];
function applyMockPins(list) {
  return list.map((s) => ({ ...s, wheelCustomized: MOCK_WHEEL_CUSTOMIZED, pinned: MOCK_PINS.indexOf(s.id) + 1 }));
}

const MOCK_CATALOG = [
  ...MOCK_EMOTES,
  { id: "solo.calibration", title: "Solo Calibration", tags: ["test", "solo", "free"], actorCount: 1, genders: ["any"], requiresFurniture: false, shape: { kind: "linear", stages: 1, nodes: 1, branches: 0 }, policy: { stripActors: false, lockPlayer: false, fade: false, camera: "none" }, priority: 1, weight: 6, sourceFile: "Data/OSF/Scenes/test.osf.json" },
  { id: "ge.chair.love", title: "GE Chair Love", tags: ["ge", "chair", "mf", "paired"], actorCount: 2, roles: [{ name: "bottom", gender: "female" }, { name: "top", gender: "male" }], requiresFurniture: true, anchors: ["Chair"], stageCount: 4, stages: [{ index: 0, name: "Missionary06", tags: ["missionary", "paired"], clipCount: 2, loopSec: 18.7, loops: 0, openEnded: true, estSec: 37.3 }, { index: 1, name: "Cowgirl07", tags: ["cowgirl", "paired"], clipCount: 2, loopSec: 20, loops: 0, openEnded: true, estSec: 40 }, { index: 2, name: "Doggy04", tags: ["doggy", "paired"], clipCount: 2, loopSec: 16, loops: 0, openEnded: true, estSec: 32 }, { index: 3, name: "Scissors02", tags: ["scissors", "paired"], clipCount: 2, loopSec: 20, loops: 0, openEnded: true, estSec: 40 }], estSec: 149.3, estPartial: false, openEnded: true, priority: 2, weight: 40, sourceFile: "Data/OSF/GE/chair.osf.json" },
  { id: "ge.akbunk.sequence", title: "GE AkBunkBed (sequence)", tags: ["ge", "akbunkbed", "mf", "paired", "sequence"], actorCount: 2, roles: [{ name: "left", gender: "female" }, { name: "right", gender: "male" }], requiresFurniture: true, anchors: ["Ak Bunk Bed"], stageCount: 5, stages: [{ index: 0, name: "Blowjob09", tags: ["blowjob", "paired"], clipCount: 2, loopSec: 18.7, loops: 2, estSec: 37.3 }, { index: 1, name: "Cowgirl06", tags: ["cowgirl", "paired"], clipCount: 2, loopSec: 20, loops: 2, estSec: 40 }, { index: 2, name: "Doggy17", tags: ["doggy", "paired"], clipCount: 2, loopSec: 20, loops: 2, estSec: 40 }, { index: 3, name: "Missionary18", tags: ["missionary", "paired"], clipCount: 2, loopSec: null, loops: 2, estSec: null }, { index: 4, name: "ReverseCowgirl23", tags: ["reversecowgirl", "paired"], clipCount: 2, timerSec: 30, loops: 0, estSec: 30 }], estSec: 147.3, estPartial: true, openEnded: false, priority: 3, weight: 25, sourceFile: "Data/OSF/GE/akbunk.osf.json" },
  { id: "pair.freeform", title: "Pair Freeform", tags: ["paired", "free", "demo"], actorCount: 2, genders: ["any", "any"], requiresFurniture: false, shape: { kind: "linear", stages: 3, nodes: 3, branches: 0 }, priority: 1, weight: 22, sourceFile: "Data/OSF/Scenes/demo.osf.json" },
  { id: "author.quest.finale", title: "Quest Finale Branch Test", tags: ["finale", "story", "branching"], actorCount: 2, roles: [{ name: "lead", gender: "any" }, { name: "partner", gender: "any" }], requiresFurniture: false, unlisted: true, shape: { kind: "graph", stages: 3, nodes: 4, branches: 3 }, policy: { stripActors: false, lockPlayer: true, fade: true, camera: "thirdperson_hold" }, priority: 2, weight: 14, sourceFile: "Data/OSF/Author/finale.osf.json" },
];
const MOCK_ACTORS = [
  { token: 601, name: "Sarah Morgan", formId: 0x2, distance: 2, isActor: true, species: "human" },
  { token: 602, name: "Andreja", formId: 0x3, distance: 5, isActor: true, species: "human" },
  { token: 603, name: "Sam Coe", formId: 0x4, distance: 9, isActor: true, species: "human" },
  { token: 605, name: "Terrormorph", formId: 0x6, distance: 7, isActor: true, species: "terrormorph" },
  { token: 604, name: "Settled Systems Citizen", formId: 0x5, distance: 12, isActor: true, species: "human" },
];
const MOCK_ANCHORS = [
  // sceneCount = total accepting scenes; customCount = the authored subset (rest are vanilla library).
  { token: 501, name: "Industrial Chair", formId: 0x12a57a, distance: 3, isActor: false, sceneCount: 2, customCount: 1 },
  { token: 502, name: "Ak Bunk Bed", formId: 0x1234, distance: 6, isActor: false, sceneCount: 1, customCount: 1 },
  { token: 503, name: "Lean Wall", formId: 0x2234, distance: 4, isActor: false, sceneCount: 2, customCount: 1, marker: true },
  { token: 504, name: "Ground Sit", formId: 0x2235, distance: 8, isActor: false, sceneCount: 1, customCount: 0, marker: true },
  { token: 505, name: "Counter Work", formId: 0x2236, distance: 11, isActor: false, sceneCount: 0, customCount: 0, marker: true },
];
const MOCK_ANCHOR_MATCH = { 501: ["ge.chair.love", "vanilla/furniture/chair"], 502: ["ge.akbunk.sequence"], 503: ["ge.chair.love", "vanilla/furniture/bench"], 504: ["vanilla/furniture/bench"], 505: [] };
const MOCK_LIBRARY = [
  { id: "vanilla/furniture/chair", title: "Vanilla · Furniture / Chair", tags: ["vanilla", "furniture", "anchored"], actorCount: 1, genders: ["any"], requiresFurniture: true, anchors: ["Chair"], sourceFile: "Data/OSF/vanilla/vanilla-furniture.osf.json", stages: [{ index: 0, name: "Idle", tags: [], clipCount: 1, loopSec: 2.7, loops: 0, openEnded: true, estSec: 5.4 }, { index: 1, name: "Idle_Flavor01", tags: ["transition"], clipCount: 1, loopSec: 11, loops: 0, openEnded: true, estSec: 22 }, { index: 2, name: "EnterFromStand", tags: ["transition", "rootmotion"], clipCount: 1, loopSec: 7.3, loops: 0, openEnded: true, estSec: 14.7 }], estSec: 42.1, estPartial: false, openEnded: true, priority: 0, weight: 1 },
  { id: "vanilla/furniture/bench", title: "Vanilla · Furniture / Bench", tags: ["vanilla", "furniture", "anchored"], actorCount: 1, genders: ["any"], requiresFurniture: true, anchors: ["Bench"], sourceFile: "Data/OSF/vanilla/vanilla-furniture.osf.json", stages: [{ index: 0, name: "Idle", tags: [], clipCount: 1, loopSec: 3.1, loops: 0, openEnded: true, estSec: 6.2 }], estSec: 6.2, estPartial: false, openEnded: true, priority: 0, weight: 1 },
  { id: "vanilla/photomode", title: "Vanilla · Photomode", tags: ["vanilla", "photomode"], actorCount: 1, genders: ["any"], requiresFurniture: false, sourceFile: "Data/OSF/vanilla/vanilla-photomode.osf.json", stages: [{ index: 0, name: "Vehicle_BackSeat", tags: [], clipCount: 1, loopSec: 0.3, loops: 0, openEnded: true, estSec: 0.6 }, { index: 1, name: "Vehicle_HangTen", tags: [], clipCount: 1, loopSec: 0.3, loops: 0, openEnded: true, estSec: 0.6 }], estSec: 1.2, estPartial: false, openEnded: true, priority: 0, weight: 1 },
  { id: "vanilla/common", title: "Vanilla · Common", tags: ["vanilla", "common"], actorCount: 1, genders: ["any"], requiresFurniture: false, sourceFile: "Data/OSF/vanilla/vanilla-common.osf.json", stages: [{ index: 0, name: "Cower_Idle", tags: ["idle"], clipCount: 1, loopSec: 6.3, loops: 0, openEnded: true, estSec: 12.7 }], estSec: 12.7, estPartial: false, openEnded: true, priority: 0, weight: 1 },
  { id: "vanilla/creature/terrormorph", title: "Vanilla · Terrormorph", species: "terrormorph", tags: ["vanilla", "creature", "species:terrormorph"], actorCount: 1, genders: ["any"], requiresFurniture: false, sourceFile: "Data/OSF/vanilla/vanilla-creature-terrormorph.osf.json", stages: [{ index: 0, name: "BleedOut_Idle", tags: ["idle"], clipCount: 1, loopSec: 8.3, loops: 0, openEnded: true, estSec: 16.6 }, { index: 1, name: "Attack_Lunge", tags: ["rootmotion"], clipCount: 1, loopSec: 2.1, loops: 0, openEnded: true, estSec: 4.2 }], estSec: 20.8, estPartial: false, openEnded: true, priority: 0, weight: 1 },
];

// Standalone live data: committed snapshot fixtures in the view's live/ folder
// (library.json regenerated by tools/generate-library-snapshot.py, catalog.json a
// one-time in-game dump). Returns the parsed array, or null (no live/ files /
// file:// page) — callers fall back to the mocks.
function fetchLiveSnapshot(name) {
  return fetch(`live/${name}.json`, { cache: "no-store" })
    .then((r) => (r.ok ? r.json() : null))
    .then((data) => (Array.isArray(data) ? data : null))
    .catch(() => null);
}

// Standalone live-scene table (the DLL's SceneRuntime slot table, mocked).
let MOCK_ACTIVE = [];
let mockHandle = 42;
function mockPushActive() {
  onNativeMessage(JSON.stringify({ type: "osf.animation.activeScenes", payload: { scenes: MOCK_ACTIVE } }));
}

function mockNative(command, fields) {
  if (command === "osf.animation.catalog.get") {
    if (wheelDbg.active) handleCatalog(dbgCatalog());  // debug override holds until RESET
    else fetchLiveSnapshot("catalog").then((live) => handleCatalog(applyMockPins(live || MOCK_CATALOG)));
  }
  else if (command === "osf.animation.library.get") fetchLiveSnapshot("library").then((live) => handleLibrary(live || MOCK_LIBRARY));
  else if (command === "osf.animation.anchorMatch") setTimeout(() => handleAnchorMatch({ token: fields.token, sceneIds: MOCK_ANCHOR_MATCH[fields.token] || [] }), 70);
  else if (command === "osf.animation.pickCrosshair") { const item = fields.slot === "furniture" ? MOCK_ANCHORS[0] : MOCK_ACTORS[0]; setTimeout(() => handlePick({ slot: fields.slot, valid: true, ...item }), 60); }
  // Mirror the DLL's open-time seed: `?target=actor|furniture` mocks a crosshair target at open.
  else if (command === "osf.animation.opened") {
    const want = new URLSearchParams(location.search).get("target");
    if (want === "actor" || want === "furniture") {
      const item = want === "furniture" ? MOCK_ANCHORS[0] : MOCK_ACTORS[0];
      setTimeout(() => handleOpenTarget({ slot: want, ...item }), 60);
    }
  }
  else if (command === "osf.animation.scanNearby") setTimeout(() => handleScanResults({ kind: fields.kind, items: fields.kind === "furniture" ? MOCK_ANCHORS : MOCK_ACTORS }), 80);
  else if (command === "osf.animation.launch") {
    if (fields.sceneId === "emote.facepalm") setTimeout(() => handleLaunchResult({ ok: false, error: "No room in front of the actor (mock error)." }), 80);
    else {
      // Mirror the DLL: replace-in-place on busy cast, then a launchResult followed by an
      // activeScenes push (multiple scenes accumulate when the casts are disjoint).
      const tokens = fields.castTokens || [];
      MOCK_ACTIVE = MOCK_ACTIVE.filter((s) => !(s.cast || []).some((c) => tokens.includes(c.token)));
      const handle = mockHandle++;
      MOCK_ACTIVE.push({
        handle, sceneId: fields.sceneId, stage: (fields.opts && fields.opts.stage) || 0,
        player: tokens.includes(PLAYER_TOKEN),
        cast: tokens.map((t) => t === PLAYER_TOKEN
          ? { token: t, name: "Player", player: true }
          : { token: t, name: (MOCK_ACTORS.find((a) => a.token === t) || {}).name || "actor", player: false }),
      });
      setTimeout(() => handleLaunchResult({ ok: true, handle, sceneId: fields.sceneId, stage: fields.opts && fields.opts.stage }), 80);
      setTimeout(() => mockPushActive(), 130);
    }
  }
  else if (command === "osf.animation.stop") {
    MOCK_ACTIVE = MOCK_ACTIVE.filter((s) => s.handle !== fields.handle);
    setTimeout(() => { notice("ok", "Scene stopped."); mockPushActive(); }, 40);
  }
  // Visibility relays from the view itself: like the DLL, closing aborts only PLAYER
  // scenes (NPC-only ones persist for the next open), and opening re-pushes the list.
  else if (command === "osf.animation.closed") MOCK_ACTIVE = MOCK_ACTIVE.filter((s) => !s.player);
  else if (command === "osf.animation.opened") setTimeout(() => mockPushActive(), 50);
  // Complete wheel loadout update, then an unsolicited catalog re-push like the DLL.
  else if (command === "osf.animation.wheel.set") {
    MOCK_WHEEL_CUSTOMIZED = !fields.reset;
    MOCK_PINS = fields.reset ? [] : (fields.sceneIds || []).slice(0, WHEEL_MAX);
    setTimeout(() => fetchLiveSnapshot("catalog").then((live) => handleCatalog(applyMockPins(live || MOCK_CATALOG))), 60);
  }
  // Host close: mimic OSF UI hiding the overlay so the real exit path runs (ui.visibility
  // hide -> exitWheel + osf.closed relay). Standalone just lands back on the console.
  else if (command === "osf.animation.requestClose") setTimeout(() => onNativeMessage(JSON.stringify({ type: "ui.visibility", payload: { visible: false } })), 60);
}

/* =========================================================================
   WHEEL DEBUG (standalone only — the strip is injected only when no bridge
   is present, so none of this can surface in-game)
   A small strip shown while the wheel is up: step the slice count through
   wheelGeom's whole range (0 = empty state, >12 proves the hard cap), toggle
   the pinned-pool ordering, the target hub label, the hub error, and the
   "Loading emotes…" state — every wheel state without in-game round-trips.
   `?wheel` in the URL boots straight into wheel mode (see init).
   ========================================================================= */
const wheelDbg = { active: false, count: 12, target: true, pins: false, err: false, loading: false };

// Debug pool: the 14 mock emotes, cycled with numbered titles past 14 so any
// count is reachable (hard-cap behavior, crowding at 12, single slice).
function dbgEmotePool(n) {
  const out = [];
  for (let i = 0; i < n; i++) {
    const base = MOCK_EMOTES[i % MOCK_EMOTES.length];
    out.push(i < MOCK_EMOTES.length ? base : { ...base, id: `${base.id}.${i}`, title: `${base.title} ${Math.floor(i / MOCK_EMOTES.length) + 1}` });
  }
  return out;
}

// PINS×3 pins the first three emotes in REVERSE catalog order (pinned 3,2,1),
// so the wheel visibly re-sorts — proving the pin-order pool beats tag order.
function dbgCatalog() {
  const list = dbgEmotePool(wheelDbg.count);
  return list.map((s, i) => ({ ...s, wheelCustomized: wheelDbg.pins, pinned: wheelDbg.pins && i < 3 ? 3 - i : 0 }));
}

function initWheelDebug() {
  const el = document.createElement("div");
  el.id = "wheeldbg";
  el.className = "wheeldbg";
  document.body.appendChild(el);
  el.addEventListener("click", (e) => {
    const b = e.target.closest("[data-dbg]");
    if (!b) return;
    e.stopPropagation();  // keep strip clicks out of the document-level wheel handlers
    dbgAct(b.dataset.dbg);
  });
  renderWheelDbg();
}

function dbgAct(act) {
  const w = state.wheel;
  if (act === "less" || act === "more") {
    if (!wheelDbg.active) { wheelDbg.active = true; wheelDbg.count = 12; }
    else wheelDbg.count = Math.max(0, Math.min(24, wheelDbg.count + (act === "more" ? 1 : -1)));
  } else if (act === "pins") {
    wheelDbg.pins = !wheelDbg.pins;
    if (wheelDbg.pins && !wheelDbg.active) { wheelDbg.active = true; wheelDbg.count = 14; }
  } else if (act === "target") {
    wheelDbg.target = !wheelDbg.target;
    if (w) w.target = wheelDbg.target ? { token: 601, name: "Sarah Morgan" } : null;
  } else if (act === "err") {
    wheelDbg.err = !wheelDbg.err;
    if (w) { w.error = wheelDbg.err ? "No room in front of the actor (debug)." : ""; w.launching = ""; }
  } else if (act === "loading") {
    wheelDbg.loading = !wheelDbg.loading;
    if (!wheelDbg.loading) state.catalogReceived = true;  // the catalog is still in state
  } else if (act === "reset") {
    Object.assign(wheelDbg, { active: false, count: 12, pins: false, err: false, loading: false });
    if (w) { w.error = ""; w.launching = ""; }
    requestCatalog(true);  // back to the live snapshot / mock catalog
  }
  if (wheelDbg.active) handleCatalog(dbgCatalog());
  state.catalogReceived = state.catalogReceived && !wheelDbg.loading;
  if (state.wheel) renderWheel();
  renderWheelDbg();
}

function renderWheelDbg() {
  const el = $("wheeldbg");
  if (!el) return;
  const on = (b) => (b ? "on" : "");
  el.innerHTML =
    `<span class="wheeldbg-title">WHEEL DEBUG</span>` +
    `<button data-dbg="less" title="Fewer slices">−</button>` +
    `<span class="wheeldbg-n">${wheelDbg.active ? wheelDbg.count : "live"}</span>` +
    `<button data-dbg="more" title="More candidates (wheel remains capped at 12)">+</button>` +
    `<button class="${on(wheelDbg.pins)}" data-dbg="pins" title="Pin the first 3 emotes in reverse order">PINS×3</button>` +
    `<button class="${on(wheelDbg.target)}" data-dbg="target" title="Hub target: Sarah / You">TARGET</button>` +
    `<button class="${on(wheelDbg.err)}" data-dbg="err" title="Hub launch error">ERROR</button>` +
    `<button class="${on(wheelDbg.loading)}" data-dbg="loading" title="Catalog not yet received">LOADING</button>` +
    `<button data-dbg="reset" title="Back to the real catalog">RESET</button>`;
}

/* =========================================================================
   DEV BACKDROP (standalone only — injected only when no bridge exists)
   In-game the page body is transparent and the live game world shows through;
   a desktop browser renders that as flat white/black, which lies about
   contrast. This stands a fake world behind the overlay: procedural scenes
   (dark ship interior / bright daylight — the readability worst case / night
   exterior / flat none), cycled with B and sticky per-tab. Dropping a real
   screenshot at live/backdrop.jpg (git-ignored, never packaged) adds it as a
   "shot" scene and makes it the default.
   ========================================================================= */
function initDevBackdrop() {
  const el = document.createElement("div");
  el.id = "devbackdrop";
  document.body.prepend(el);
  const scenes = ["interior", "day", "night", "none"];
  const apply = (scene) => {
    el.dataset.scene = scene;
    el.style.backgroundImage = scene === "shot" ? "url(live/backdrop.jpg)" : "";
    try { sessionStorage.osfDevBackdrop = scene; } catch {}
  };
  const saved = (() => { try { return sessionStorage.osfDevBackdrop; } catch { return null; } })();
  apply(scenes.includes(saved) ? saved : "interior");
  // A user screenshot beats the procedural scenes: joins the cycle and wins by default.
  const img = new Image();
  img.onload = () => {
    scenes.unshift("shot");
    if (!scenes.includes(saved) || saved === "shot") apply("shot");  // a saved procedural pick stands
  };
  img.src = "live/backdrop.jpg";
  document.addEventListener("keydown", (e) => {
    if (isTextEntry(document.activeElement)) return;
    if (e.key !== "b" && e.key !== "B") return;
    apply(scenes[(scenes.indexOf(el.dataset.scene) + 1) % scenes.length]);
  });
}

init();
