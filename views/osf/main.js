// OSF Animation - Scene Director. NASA-punk maintenance-HUD console.
// Guided pre-flight flow: 1 · CAST (pick actors) → 2 · ANCHOR (optional furniture, shows what
// it unlocks) → BROWSE (only what plays with the current selection, plus the vanilla library).
// Wired to the unchanged OSF UI bridge contract: only JSON text crosses window.osfui.
"use strict";

const BRIDGE_PROTOCOL = "0.1";
const PLAYER_TOKEN = -1;
const PLAYER_CAST = { token: PLAYER_TOKEN, name: "Player", kind: "player" };

const state = {
  ready: false,
  catalog: [],
  allUnlisted: false,
  selectedId: null,
  partners: [],
  furniture: null,
  nearbyActors: [],
  nearbyFurniture: [],
  lastHandle: 0,
  lastSceneId: "",
  opts: { strip: "-1", lock: "-1", camera: "", speed: "1" },
  filters: { search: "", authorMode: false },
  catalogReceived: false,
  // Which anchor-bound scenes the KEYED furniture actually fits (osf.anchorMatch reply).
  // null until known; browse falls back to "any anchor present" while the reply is in flight.
  anchorMatch: null,  // { token, ids: Set<string> }
  browseAll: false,   // scenes mode: false = playable now only, true = everything
  // Reference-library lane (osf.library.*): the generated vanilla packs. Static after load,
  // fetched once on demand when the LIBRARY mode is first opened, then cached for the session.
  mode: "scenes",  // "scenes" | "library"
  library: [],
  libraryReceived: false,
  libOpen: new Set(),  // expanded library pack groups
  libShowAll: false,   // library: false = focus on sets fitting the keyed anchor, true = all
  markersOpen: false,  // anchor step: the collapsed "AI markers" group (invisible idle markers)
  // Collapsible pre-flight steps: once cast/anchor are set they fold to a summary line so
  // the browse list gets the dock's height back (the rail otherwise eats ~half the column).
  stepOpen: { cast: true, anchor: true },
};

const $ = (id) => document.getElementById(id);

/* =========================================================================
   BRIDGE  (unchanged contract — osf.* over window.osfui)
   ========================================================================= */
function bridgeAvailable() {
  return typeof window.osfui === "object" && typeof window.osfui.postMessage === "function";
}

function send(command, fields = {}) {
  const msg = JSON.stringify({ type: "ui.command", payload: { command, ...fields } });
  if (bridgeAvailable()) window.osfui.postMessage(msg);
  else { console.log("(standalone) ->", msg); mockNative(command, fields); }
}

let catalogTimer = null;
let catalogTries = 0;
const CATALOG_MAX_TRIES = 20;

function requestCatalog(fresh) {
  if (fresh) { state.catalogReceived = false; catalogTries = 0; }
  clearTimeout(catalogTimer);
  if (state.catalogReceived) return;
  send("osf.catalog.get");
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
  send("osf.library.get");
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
    case "osf.catalog.data": handleCatalog(payload); break;
    case "osf.library.data": handleLibrary(payload); break;
    case "osf.pick": handlePick(payload); break;
    case "osf.scanResults": handleScanResults(payload); break;
    case "osf.anchorMatch": handleAnchorMatch(payload); break;
    case "osf.launchResult": handleLaunchResult(payload); break;
    // The runtime tells the focused view when the overlay shows; report each open so the
    // plugin's first-run "press F10" hint can count real opens and retire itself.
    case "ui.visibility": if (payload && payload.visible) send("osf.opened"); break;
    default: break;
  }
}

window.osfui = window.osfui || {};
window.osfui.onMessage = onNativeMessage;

function handleReady(p) {
  const bv = p && p.bridgeVersion;
  if (!bv || !bv.startsWith("0.")) {
    setLamp("off");
    $("statusText").textContent = `unsupported bridge ${bv || "?"}`;
    notice("err", `This view needs bridge protocol ${BRIDGE_PROTOCOL}; runtime reports ${bv || "?"}.`);
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
  state.allUnlisted = state.catalog.length > 0 && state.catalog.every((s) => s.unlisted);
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
    notice("err", `Nothing valid under the crosshair for the ${(p && p.slot) || "target"} slot.`);
    return;
  }
  applyPick(p.slot === "furniture" ? "furniture" : "actor", p.token, p.name, p.distance);
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
    sceneCount: typeof it.sceneCount === "number" ? it.sceneCount : null,  // furniture: total scenes it unlocks
    customCount: typeof it.customCount === "number" ? it.customCount : null,  // furniture: subset that is custom (authored), not vanilla library
    marker: !!it.marker,  // invisible AI/idle marker — a usable anchor, listed in its own group
  }));
  if (kind === "furniture") {
    state.nearbyFurniture = normalized;
    const markers = normalized.filter((x) => x.marker).length;
    const named = normalized.length - markers;
    notice("info", `${named} usable anchor${named === 1 ? "" : "s"} found${markers ? ` + ${markers} AI marker${markers === 1 ? "" : "s"}` : ""}.`);
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

function handleLaunchResult(p) {
  if (p && p.ok && p.handle) {
    state.lastHandle = p.handle;
    state.lastSceneId = p.sceneId || state.selectedId || "";
    notice("ok", `Playing "${sceneTitle(state.lastSceneId)}" on handle ${p.handle}.`);
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
  const requiresFurniture = !!(raw.requiresFurniture || raw.anchorRequired || raw.anchor);
  return {
    id,
    title: String(raw.title || raw.name || id || "Unnamed scene"),
    tags,
    actorCount,
    genders,
    roles,
    requiresFurniture,
    unlisted: !!raw.unlisted,
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
  state.furniture = { token, name: name || "anchor", distance: distance || null };
  state.anchorMatch = null;               // stale until the engine answers for THIS token
  state.libShowAll = false;               // re-focus the library on what this anchor fits
  state.stepOpen.anchor = false;          // step done — fold it, give the browse list the height back
  send("osf.anchorMatch", { token });     // which anchor-bound scenes does it fit?
  notice("info", `Anchor keyed: ${state.furniture.name}.`);
}

function clearAnchor() {
  state.furniture = null;
  state.anchorMatch = null;
}

function applyPick(slot, token, name, distance) {
  if (slot === "furniture") {
    keyAnchor(token, name, distance);
  } else if (token === PLAYER_TOKEN || state.partners.some((c) => c.token === token)) {
    notice("info", `${name || "Actor"} is already in the cast.`);
  } else {
    state.partners.push({ token, name: name || "actor", distance: distance || null });
    state.stepOpen.cast = false;  // partner added — fold, freeing the browse list's height (mirrors keyAnchor)
    notice("info", `Cast added: ${name || "actor"}.`);
  }
  renderAll();
}

function toggleActor(token) {
  const idx = state.partners.findIndex((p) => p.token === token);
  if (idx >= 0) { state.partners.splice(idx, 1); notice("info", "Cast member removed."); }
  else {
    const a = state.nearbyActors.find((x) => x.token === token);
    if (a) { state.partners.push({ token: a.token, name: a.name, distance: a.distance }); state.stepOpen.cast = false; notice("info", `Cast added: ${a.name}.`); }
  }
  renderAll();
}

function toggleAnchor(token) {
  if (state.furniture && state.furniture.token === token) { clearAnchor(); notice("info", "Anchor cleared."); }
  else {
    const an = state.nearbyFurniture.find((x) => x.token === token);
    if (an) keyAnchor(an.token, an.name, an.distance);
  }
  renderAll();
}

function removePartner(index) { state.partners.splice(index, 1); renderAll(); }

function scanNearby(kind) {
  notice("info", `Scanning nearby ${kind === "furniture" ? "anchors" : "actors"}…`);
  send("osf.scanNearby", { kind, sceneId: state.selectedId || "" });
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

function activeList() { return state.mode === "library" ? state.library : state.catalog; }

function applySelection(id) { state.selectedId = id; }
function selectScene(id) { applySelection(id); renderAll(); }
function sceneById(id) { return state.catalog.find((s) => s.id === id) || state.library.find((s) => s.id === id) || null; }
function sceneTitle(id) { const s = sceneById(id); return s ? s.title : (id || "scene"); }
function castTokens() { return [PLAYER_TOKEN, ...state.partners.map((c) => c.token)]; }
function castMembers() { return [PLAYER_CAST, ...state.partners]; }

function unlistedVisible(s) { return !!s && (!s.unlisted || state.filters.authorMode || state.allUnlisted); }

function ensureSelection() {
  // Track the fully-filtered pool of the ACTIVE mode so the brief never inspects a scene no
  // row shows. Re-pick to the top visible scene if the current selection is filtered out.
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
  if (overCast) { const n = castCount - actorCount; blockers.push(`remove ${n} cast member${n === 1 ? "" : "s"}`); }
  if (!anchorGate) issues.push(state.furniture ? "keyed anchor doesn't fit" : "needs an anchor");
  const gaps = issues.length + blockers.length;
  const reason = gaps === 0 ? "Ready with the current cast and anchor." : [...issues, ...blockers].map(sentenceCase).join(". ") + ".";
  return { castCount, actorCount, hasRoles, rolesGate, overCast, anchorGate, seated, issues, blockers, gaps, reason };
}

function needsText(s, ev) {
  if (!ev.rolesGate) { const n = ev.actorCount - ev.castCount; return `+${n} actor${n === 1 ? "" : "s"}`; }
  if (!ev.anchorGate) return state.furniture ? "other anchor" : "needs anchor";
  if (ev.overCast) { const n = ev.castCount - ev.actorCount; return `-${n} cast`; }
  return "";
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

// What the browse column shows for the current mode/filters (used by ensureSelection too).
function browseVisible(s) {
  if (!matchesSearch(s)) return false;
  if (state.mode === "scenes" && !state.browseAll) return evalScene(s).gaps === 0;
  return true;
}

/* =========================================================================
   RENDER
   ========================================================================= */
function renderAll() {
  ensureSelection();
  renderSlateTake();
  renderRail();
  renderBrowse();
  renderBrief();
}

/* ---- slate: live take ----------------------------------------------------- */
function renderSlateTake() {
  $("authorToggle").classList.toggle("on", state.filters.authorMode);
  const el = $("slateTake");
  if (state.lastHandle) {
    const ls = sceneById(state.lastSceneId);
    el.innerHTML = `<div class="take-chip live"><span class="live-dot"></span><div class="take-body"><span class="lbl">LIVE TAKE #${state.lastHandle}</span><strong>${esc(ls ? ls.title : state.lastSceneId)}</strong></div><button class="stop-mini" data-act="stop" title="Stop the running scene">■ STOP</button></div>`;
  } else {
    el.innerHTML = `<div class="take-chip"><span class="lbl">NO TAKE RUNNING</span><span class="mono">cast → anchor → launch</span></div>`;
  }
}

/* ---- guided rail ---------------------------------------------------------- */
function renderRail() {
  $("rail").innerHTML = stepCastHTML() + stepAnchorHTML();
}

// Clickable step header; a collapsed step folds to this single line (note = the summary).
function stepHeadHTML(step, num, title, note, open) {
  return `<button class="step-head" data-act="step-toggle" data-step="${step}"><span class="step-num">${num}</span><span class="eb">${title}</span><span class="step-note">${note}</span><span class="chev">${open ? "▾" : "▸"}</span></button>`;
}

function stepCastHTML() {
  const members = castMembers();
  const castCount = members.length;
  const open = state.stepOpen.cast;
  if (!open) {
    // Folded: who's on set, still readable at a glance.
    const names = members.map((m) => m.name).join(" + ");
    return `<div class="step closed">${stepHeadHTML("cast", 1, "CAST", esc(castCount === 1 ? "Player only" : names), false)}</div>`;
  }

  const chips = members.map((m, i) => {
    const player = m.kind === "player";
    const drop = player ? "" : `<button class="chip-x" data-act="drop" data-i="${i - 1}" title="Remove from cast">×</button>`;
    return `<span class="castline ${player ? "player" : ""}"><span class="cast-key">${String.fromCharCode(65 + i)}</span><span class="castline-name">${esc(m.name)}</span>${drop}</span>`;
  }).join("");

  const rows = state.nearbyActors.length
    ? state.nearbyActors.map((a) => {
        const added = state.partners.some((p) => p.token === a.token);
        return `<button class="near-row ${added ? "active" : ""}" data-act="toggle-actor" data-token="${a.token}"><span class="near-name">${esc(a.name)}</span><span class="near-meta mono">${a.distance != null ? Math.max(1, Math.round(a.distance)) + "m" : ""}</span><span class="near-tag ${added ? "added" : ""}">${added ? "✓" : "ADD"}</span></button>`;
      }).join("")
    : `<div class="empty-mini"><span class="mono">Scan, or aim at someone and PICK.</span></div>`;

  const fit = state.catalog.filter((s) => unlistedVisible(s) && (s.actorCount || 0) === castCount).length;
  const libNote = castCount === 1 ? (state.libraryReceived ? ` · library ${state.library.length}` : " · + library") : "";
  const foot = state.catalogReceived
    ? `<div class="step-foot"><span class="mono">${fit} scene${fit === 1 ? "" : "s"} fit ${castCount} actor${castCount === 1 ? "" : "s"}${libNote}</span></div>`
    : "";

  return `<div class="step">
    ${stepHeadHTML("cast", 1, "CAST", `${castCount} on set`, true)}
    <div class="cast-stack">${chips}</div>
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
    unlocks = `<span class="near-badge ${custom ? "" : "empty"}" title="Custom scenes this unlocks">⚓${custom}</span>`;
    if (lib) unlocks += `<span class="near-badge lib" title="Vanilla library scenes this unlocks">+${lib}</span>`;
  } else if (total != null) {
    unlocks = `<span class="near-badge" title="Anchored scenes this unlocks">⚓${total}</span>`;
  }
  return `<button class="near-row ${active ? "active" : ""} ${an.marker ? "marker" : ""}" data-act="toggle-anchor" data-token="${an.token}"><span class="near-name">${esc(an.name)}</span>${unlocks}<span class="near-meta mono">${an.distance != null ? Math.max(1, Math.round(an.distance)) + "m" : ""}</span><span class="near-tag ${active ? "added" : ""}">${active ? "✓" : "USE"}</span></button>`;
}

function stepAnchorHTML() {
  const keyed = state.furniture;
  const matchKnown = !!(keyed && state.anchorMatch && state.anchorMatch.token === keyed.token);
  if (!state.stepOpen.anchor) {
    return `<div class="step closed">${stepHeadHTML("anchor", 2, "ANCHOR", keyed ? `⚓ ${esc(keyed.name)}` : "none", false)}</div>`;
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
    rows += `<button class="reveal ${open ? "on" : ""}" data-act="markers-toggle" title="Invisible AI idle spots — valid anchors, but you can't see them in the world">${open ? "▾" : "▸"} ${markers.length} AI marker${markers.length === 1 ? "" : "s"} (invisible)</button>`;
    if (open) rows += markers.map((an) => anchorRow(an, keyed)).join("");
  }

  const foot = keyed
    ? `<div class="step-foot"><span class="mono">${matchKnown ? anchorFitLabel() : "checking what fits…"}</span></div>`
    : "";

  return `<div class="step">
    ${stepHeadHTML("anchor", 2, "ANCHOR", "optional", true)}
    ${slot}
    <div class="step-sub"><span class="lbl">NEARBY</span><span class="step-tools"><button class="chip-btn" data-act="scan" data-kind="furniture">SCAN</button><button class="chip-btn" data-act="pick" data-slot="furniture">PICK</button></span></div>
    <div class="near-list">${rows}</div>
    ${foot}
  </div>`;
}

/* ---- browse ---------------------------------------------------------------- */
function renderBrowse() {
  const libCount = state.libraryReceived ? String(state.library.length) : "…";
  $("modeSwitch").innerHTML =
    `<button class="mode-btn ${state.mode === "scenes" ? "on" : ""}" data-act="mode" data-mode="scenes">SCENES · ${state.catalog.length}</button>` +
    `<button class="mode-btn ${state.mode === "library" ? "on" : ""}" data-act="mode" data-mode="library">LIBRARY · ${libCount}</button>`;
  $("browseBody").innerHTML = state.mode === "library" ? libraryBrowserHTML() : scenesBrowserHTML();
}

function scenesBrowserHTML() {
  if (!state.catalogReceived) return bayEmpty("Waiting for the catalog…");
  // Fresh install with no scene packs: the vanilla library IS the out-of-box content — route there.
  if (state.catalog.length === 0) {
    return `<div class="bay-empty"><span class="mono">No scene packs installed — the built-in animation library still has plenty to play.</span><button class="chip-btn" data-act="mode" data-mode="library" style="margin-top:8px">OPEN LIBRARY ▸</button></div>`;
  }
  const searched = state.catalog.filter(matchesSearch);
  const evald = searched.map((s) => ({ s, ev: evalScene(s) }));
  const playable = evald.filter((x) => x.ev.gaps === 0);
  const rest = evald.filter((x) => x.ev.gaps > 0);
  // With an anchor keyed, float the scenes that actually fit that furniture above the
  // free-space ones — so keying a chair surfaces the chair scenes first, not last.
  const anchorFit = (x) => !!(state.furniture && x.s.requiresFurniture && x.ev.anchorGate);
  const byRank = (a, b) => (anchorFit(b) - anchorFit(a)) || b.s.priority - a.s.priority || b.s.weight - a.s.weight || a.s.title.localeCompare(b.s.title);
  playable.sort(byRank); rest.sort(byRank);

  let html = `<div class="browse-note"><span class="dot go"></span><span class="lbl">PLAYABLE NOW · ${playable.length}</span></div>`;
  html += playable.length
    ? `<div class="row-list">${playable.map((x) => sceneRow(x.s, x.ev, true)).join("")}</div>`
    : bayEmpty(state.furniture || state.partners.length ? "Nothing fits this exact cast + anchor. Adjust the selection, or show the rest." : "Add cast or key an anchor to unlock scenes.");
  if (rest.length) {
    html += `<button class="reveal ${state.browseAll ? "on" : ""}" data-act="browse-all">${state.browseAll ? "▾" : "▸"} ${rest.length} more need a different cast or anchor</button>`;
    if (state.browseAll) html += `<div class="row-list dim">${rest.map((x) => sceneRow(x.s, x.ev, false)).join("")}</div>`;
  }
  return html;
}

function sceneRow(s, ev, playable) {
  const sel = s.id === state.selectedId;
  const n = s.actorCount || 1;
  const dur = fmtEst(s);
  const bits = [`${n} role${n === 1 ? "" : "s"}`];
  if (s.requiresFurniture) bits.push("anchor");
  if (dur) bits.push(dur);
  const meta = state.filters.authorMode ? s.id : bits.join(" · ");
  const badge = playable ? `<span class="row-badge go">READY</span>` : `<span class="row-badge">${esc(needsText(s, ev))}</span>`;
  return `<button class="libx-row ${sel ? "selected" : ""}" data-act="select-scene" data-id="${escAttr(s.id)}"><span class="libx-spine"></span><span class="libx-title">${esc(s.title)}</span><span class="libx-meta mono">${esc(meta)}</span>${badge}</button>`;
}

/* ---- library browser (reference lane: compact folder-grouped rows) ------- */
function packKey(s) {
  const f = String(s.sourceFile || "").replace(/\\/g, "/").split("/").pop() || "";
  return f.replace(/\.osf\.json$/i, "") || "library";
}

function groupLabel(key) { return key.replace(/^vanilla-/i, "").replace(/[-_]+/g, " ").toUpperCase(); }

// null = anchor status irrelevant (free-space set, or no anchor keyed yet); true/false =
// this furniture-bound set does / doesn't fit the keyed anchor.
function fitsKeyedAnchor(s) {
  if (!s.requiresFurniture) return null;  // free-space: playable regardless of any anchor
  if (!state.furniture || !state.anchorMatch || state.anchorMatch.token !== state.furniture.token) return null;
  return state.anchorMatch.ids.has(s.id);
}

function libraryGroups(pred) {
  const m = new Map();
  for (const s of state.library) {
    if (!matchesSearch(s)) continue;
    if (pred && !pred(s)) continue;
    const key = packKey(s);
    if (!m.has(key)) m.set(key, []);
    m.get(key).push(s);
  }
  for (const list of m.values()) list.sort((a, b) => a.title.localeCompare(b.title));
  return [...m.entries()].sort((a, b) => a[0].localeCompare(b[0]));
}

function libraryBrowserHTML() {
  if (!state.libraryReceived) return bayEmpty("Loading the animation library…");
  if (!state.library.length) return bayEmpty("No library packs installed (Data/OSF/vanilla).");

  // When furniture is keyed, focus the library on sets that fit it (this is the "filter the
  // furniture animations by furniture" behavior) — with a one-click escape to the full list.
  const matchKnown = !!(state.furniture && state.anchorMatch && state.anchorMatch.token === state.furniture.token);
  const fitFocus = matchKnown && !state.libShowAll;
  const groups = libraryGroups(fitFocus ? (s) => state.anchorMatch.ids.has(s.id) : null);

  let banner;
  if (matchKnown) {
    // Count LIBRARY sets that fit (anchorMatch also covers scenes-lane scenes, which don't belong here).
    const fitCount = state.library.filter((s) => matchesSearch(s) && state.anchorMatch.ids.has(s.id)).length;
    banner = `<div class="browse-note"><span class="dot go"></span><span class="lbl">⚓ ${esc(state.furniture.name)} · ${fitCount} SET${fitCount === 1 ? "" : "S"} FIT</span>` +
      `<button class="reveal inline ${state.libShowAll ? "on" : ""}" data-act="lib-showall">${state.libShowAll ? "show fitting only" : "show all"}</button></div>`;
  } else {
    const total = state.library.length;
    const clips = state.library.reduce((a, s) => a + (s.stages || []).length, 0);
    banner = `<div class="browse-note"><span class="dot"></span><span class="lbl">ANIMATION LIBRARY · ${clips} CLIPS IN ${total} SETS</span></div>`;
  }

  if (!groups.length) {
    return banner + bayEmpty(fitFocus ? "No library sets fit the keyed anchor. Show all, or key different furniture." : "Nothing in the library matches the filter.");
  }
  const searching = !!state.filters.search;
  const body = groups.map(([key, list]) => {
    // Auto-expand when searching or an anchor is keyed (so the ⚓ fit/no-fit marks are visible).
    const open = searching || matchKnown || state.libOpen.has(key);
    const stageTotal = list.reduce((a, s) => a + (s.stages || []).length, 0);
    const rows = open ? `<div class="libx-list">${list.map(libxRow).join("")}</div>` : "";
    return `<div class="libx-group">` +
      `<button class="libx-head" data-act="lib-group" data-key="${escAttr(key)}"><span class="chev">${open ? "▾" : "▸"}</span><span class="libx-name">${esc(groupLabel(key))}</span><span class="libx-meta mono">${list.length} set${list.length === 1 ? "" : "s"} · ${stageTotal} anim${stageTotal === 1 ? "" : "s"}</span></button>` +
      rows + `</div>`;
  }).join("");
  return banner + body;
}

function libxRow(s) {
  const sel = s.id === state.selectedId;
  const n = (s.stages || []).length;
  const title = s.title.replace(/^Vanilla · /, "");
  const meta = state.filters.authorMode ? s.id : `${n} anim${n === 1 ? "" : "s"}`;
  // ⚓ marks furniture-bound sets; when an anchor is keyed, tint by whether this set fits it.
  const fits = fitsKeyedAnchor(s);
  const anchorMark = s.requiresFurniture
    ? `<span class="libx-anchor ${fits === true ? "fit" : fits === false ? "nofit" : ""}" title="Needs matching furniture">⚓</span>`
    : "";
  return `<button class="libx-row ${sel ? "selected" : ""} ${fits === false ? "dim" : ""}" data-act="select-scene" data-id="${escAttr(s.id)}"><span class="libx-spine"></span><span class="libx-title">${esc(title)}</span>${anchorMark}<span class="libx-meta mono">${esc(meta)}</span></button>`;
}

function bayEmpty(msg) { return `<div class="bay-empty"><span class="mono">${esc(msg)}</span></div>`; }

/* ---- brief (slim) --------------------------------------------------------- */
function renderBrief() {
  const brief = $("brief");
  const s = sceneById(state.selectedId);
  if (!s) { brief.innerHTML = `<div class="brief-empty"><span class="mono">Nothing selected.</span></div>`; return; }
  const ev = evalScene(s);
  const allMet = ev.gaps === 0;

  const dur = fmtEst(s);
  const head = `<div class="brief-status ${allMet ? "" : "warn"}"><span class="dot"></span><p class="eb">${allMet ? "READY TO LAUNCH" : "NOT SEATABLE YET"}</p></div>
    <div class="brief-title">${esc(s.title)}${dur ? `<span class="card-dur">${esc(dur)}</span>` : ""}</div>
    ${state.filters.authorMode ? `<div class="mono wrap brief-src">${esc(s.id)} · ${esc(s.sourceFile || "live registry")}</div>` : ""}`;

  // One readable line instead of the gauge/req/seat instrument cluster.
  const anchorBit = s.requiresFurniture
    ? (state.furniture ? (ev.anchorGate ? `anchored: ${state.furniture.name}` : `keyed anchor doesn't fit`) : "needs an anchor")
    : "free-space";
  const summary = `<div class="brief-line ${allMet ? "" : "warn"}"><span class="mono">${esc(`${ev.seated}/${ev.actorCount || "?"} cast · ${anchorBit}`)}</span></div>`;

  const stages = s.stages || [];
  const canPlay = state.ready && allMet;
  const animBox = stages.length
    ? `<div class="info-box"><div class="lbl">ANIMATIONS · ${stages.length}</div><div class="anim-list">${stages.map((st) => {
        const label = st.name || `Stage ${st.index}`;
        const tags = (st.tags || []).slice(0, 3).map((t) => `<span class="pill">${esc(t)}</span>`).join("");
        const loop = fmtDur(st.loopSec);
        const d = loop || fmtDur(st.estSec);
        const durHTML = d ? `<span class="anim-dur" title="${loop ? "Loop length" : "Stage time"}">${esc(d)}${st.openEnded ? "∞" : ""}</span>` : "";
        return `<div class="anim-row"><div class="anim-main"><span class="anim-name">${esc(label)}</span><div class="anim-tags">${tags}</div></div>${durHTML}<button class="anim-play" data-act="play-stage" data-stage="${st.index}" ${canPlay ? "" : "disabled"} title="Play this animation">▶</button></div>`;
      }).join("")}</div></div>`
    : "";

  const o = state.opts;
  const overrides = `<div class="info-box"><div class="lbl">START OVERRIDES</div><div class="override-grid"><label class="override"><span class="lbl">STRIP</span><select class="select" data-field="strip">${optionTags([["-1", "Inherit"], ["1", "On"], ["0", "Off"]], o.strip)}</select></label><label class="override"><span class="lbl">LOCK PLAYER</span><select class="select" data-field="lock">${optionTags([["-1", "Inherit"], ["1", "On"], ["0", "Off"]], o.lock)}</select></label><label class="override"><span class="lbl">CAMERA</span><select class="select" data-field="camera">${optionTags([["", "Inherit"], ["thirdperson_hold", "Third person"], ["scene_orbit", "Scene orbit"], ["freefly", "Free fly"], ["vanity_orbit", "Vanity orbit"]], o.camera)}</select></label><label class="override"><span class="lbl">SPEED <b id="speedVal">${Number(o.speed).toFixed(1)}x</b></span><input id="optSpeed" class="range" type="range" min="0.1" max="3" step="0.1" value="${escAttr(o.speed)}"></label></div></div>`;

  const authorBoxes = state.filters.authorMode
    ? `<div class="info-box hud"><div class="lbl">DIAGNOSTICS</div><div class="kv-list">${diagRows(s, ev).map(([k, v]) => `<div class="kv"><span class="k">${esc(k)}</span><span class="v">${esc(v)}</span></div>`).join("")}</div></div>`
    : "";

  const reason = !state.ready ? "Engine not connected." : (allMet ? "" : ev.reason);
  const launchBtn = canPlay
    ? `<button class="launch-btn go" data-act="launch">▶ Launch Scene</button>`
    : `<button class="launch-btn blocked" disabled>${esc(!state.ready ? "Engine Offline" : `Blocked · ${ev.gaps} gap${ev.gaps > 1 ? "s" : ""}`)}</button>`;
  const stopBtn = state.lastHandle ? `<button class="stop-btn" data-act="stop">■ Stop Take #${state.lastHandle}</button>` : "";
  const reasonHTML = reason ? `<div class="mono wrap" style="color:var(--text-faint);text-align:center">${esc(reason)}</div>` : "";
  const launchStack = `<div class="launch-stack">${reasonHTML}${launchBtn}${stopBtn}</div>`;

  // Fixed header (status/title/cast) + fixed footer (overrides + launch/stop); only the animations
  // band between them scrolls. The ▶ Launch button and its overrides stay in view no matter how many
  // stages a scene has — no more scrolling past a long list to reach the button.
  brief.innerHTML = head + summary +
    `<div class="brief-scroll">${animBox}${authorBoxes}</div>` +
    `<div class="brief-foot">${overrides}${launchStack}</div>`;
}

function diagRows(s, ev) {
  const rows = [["weight · priority", `${s.weight} · ${s.priority}`]];
  (s.roles || []).forEach((r, i) => rows.push([`role ${String.fromCharCode(65 + i)} filter`, `${r.gender || "any"} · ${ev.seated > i ? "pass" : "open"}`]));
  rows.push(["anchor", (s.requiresFurniture ? "required" : "free-space") + " · " + (ev.anchorGate ? "pass" : "fail")]);
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
  send("osf.launch", payload);
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

function doStop() {
  if (!state.lastHandle) return;
  send("osf.stop", { handle: state.lastHandle });
  notice("info", `Stopping handle ${state.lastHandle}…`);
  state.lastHandle = 0;
  renderAll();
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
    case "mode": setMode(el.dataset.mode === "library" ? "library" : "scenes"); break;
    case "lib-group": {
      const k = el.dataset.key;
      if (state.libOpen.has(k)) state.libOpen.delete(k);
      else state.libOpen.add(k);
      renderAll();
      break;
    }
    case "browse-all": state.browseAll = !state.browseAll; renderAll(); break;
    case "markers-toggle": state.markersOpen = !state.markersOpen; renderAll(); break;
    case "step-toggle": {
      const k = el.dataset.step === "anchor" ? "anchor" : "cast";
      state.stepOpen[k] = !state.stepOpen[k];
      renderAll();
      break;
    }
    case "lib-showall": state.libShowAll = !state.libShowAll; renderAll(); break;
    case "toggle-actor": toggleActor(Number(el.dataset.token)); break;
    case "toggle-anchor": toggleAnchor(Number(el.dataset.token)); break;
    case "scan": scanNearby(el.dataset.kind); break;
    case "pick": send("osf.pickCrosshair", { slot: el.dataset.slot }); break;
    case "clear-anchor": clearAnchor(); renderAll(); break;
    case "drop": removePartner(Number(el.dataset.i)); break;
    case "launch": doLaunch(); break;
    case "stop": doStop(); break;
    case "play-stage": doLaunch(Number(el.dataset.stage)); break;
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

function init() {
  // Static browse skeleton: the search input must survive re-renders (focus + caret),
  // so only #modeSwitch and #browseBody are re-rendered.
  $("browse").innerHTML = `<div class="browse-head"><div class="mode-switch" id="modeSwitch"></div><div class="search-field grow"><input id="search" type="text" placeholder="⌕ search scenes · animations · tags" autocomplete="off" spellcheck="false"></div></div><div id="browseBody" class="browse-body"></div>`;

  document.addEventListener("click", onClick);
  document.addEventListener("change", onChange);
  document.addEventListener("input", onInput);
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
    $("statusText").textContent = "standalone mock";
    state.ready = true;
    state.nearbyActors = MOCK_ACTORS.slice();
    state.nearbyFurniture = MOCK_ANCHORS.slice();
    handleCatalog(MOCK_CATALOG);
    notice("info", "Standalone mode. Mock catalog, native calls are stubbed.");
  }
}

/* =========================================================================
   MOCK (standalone dev — exercises normalizeScene / the bridge stubs)
   ========================================================================= */
const MOCK_CATALOG = [
  { id: "solo.calibration", title: "Solo Calibration", tags: ["test", "solo", "free"], actorCount: 1, genders: ["any"], requiresFurniture: false, shape: { kind: "linear", stages: 1, nodes: 1, branches: 0 }, policy: { stripActors: false, lockPlayer: false, fade: false, camera: "none" }, priority: 1, weight: 6, sourceFile: "Data/OSF/Scenes/test.osf.json" },
  { id: "ge.chair.love", title: "GE Chair Love", tags: ["ge", "chair", "mf", "paired"], actorCount: 2, roles: [{ name: "bottom", gender: "female" }, { name: "top", gender: "male" }], requiresFurniture: true, stageCount: 4, stages: [{ index: 0, name: "Missionary06", tags: ["missionary", "paired"], clipCount: 2, loopSec: 18.7, loops: 0, openEnded: true, estSec: 37.3 }, { index: 1, name: "Cowgirl07", tags: ["cowgirl", "paired"], clipCount: 2, loopSec: 20, loops: 0, openEnded: true, estSec: 40 }, { index: 2, name: "Doggy04", tags: ["doggy", "paired"], clipCount: 2, loopSec: 16, loops: 0, openEnded: true, estSec: 32 }, { index: 3, name: "Scissors02", tags: ["scissors", "paired"], clipCount: 2, loopSec: 20, loops: 0, openEnded: true, estSec: 40 }], estSec: 149.3, estPartial: false, openEnded: true, priority: 2, weight: 40, sourceFile: "Data/OSF/GE/chair.osf.json" },
  { id: "ge.akbunk.sequence", title: "GE AkBunkBed (sequence)", tags: ["ge", "akbunkbed", "mf", "paired", "sequence"], actorCount: 2, roles: [{ name: "left", gender: "female" }, { name: "right", gender: "male" }], requiresFurniture: true, stageCount: 5, stages: [{ index: 0, name: "Blowjob09", tags: ["blowjob", "paired"], clipCount: 2, loopSec: 18.7, loops: 2, estSec: 37.3 }, { index: 1, name: "Cowgirl06", tags: ["cowgirl", "paired"], clipCount: 2, loopSec: 20, loops: 2, estSec: 40 }, { index: 2, name: "Doggy17", tags: ["doggy", "paired"], clipCount: 2, loopSec: 20, loops: 2, estSec: 40 }, { index: 3, name: "Missionary18", tags: ["missionary", "paired"], clipCount: 2, loopSec: null, loops: 2, estSec: null }, { index: 4, name: "ReverseCowgirl23", tags: ["reversecowgirl", "paired"], clipCount: 2, timerSec: 30, loops: 0, estSec: 30 }], estSec: 147.3, estPartial: true, openEnded: false, priority: 3, weight: 25, sourceFile: "Data/OSF/GE/akbunk.osf.json" },
  { id: "pair.freeform", title: "Pair Freeform", tags: ["paired", "free", "demo"], actorCount: 2, genders: ["any", "any"], requiresFurniture: false, shape: { kind: "linear", stages: 3, nodes: 3, branches: 0 }, priority: 1, weight: 22, sourceFile: "Data/OSF/Scenes/demo.osf.json" },
  { id: "author.quest.finale", title: "Quest Finale Branch Test", tags: ["finale", "story", "branching"], actorCount: 2, roles: [{ name: "lead", gender: "any" }, { name: "partner", gender: "any" }], requiresFurniture: false, unlisted: true, shape: { kind: "graph", stages: 3, nodes: 4, branches: 3 }, policy: { stripActors: false, lockPlayer: true, fade: true, camera: "thirdperson_hold" }, priority: 2, weight: 14, sourceFile: "Data/OSF/Author/finale.osf.json" },
];
const MOCK_ACTORS = [
  { token: 601, name: "Sarah Morgan", formId: 0x2, distance: 2, isActor: true },
  { token: 602, name: "Andreja", formId: 0x3, distance: 5, isActor: true },
  { token: 603, name: "Sam Coe", formId: 0x4, distance: 9, isActor: true },
  { token: 604, name: "Settled Systems Citizen", formId: 0x5, distance: 12, isActor: true },
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
  { id: "vanilla/furniture/chair", title: "Vanilla · Furniture / Chair", tags: ["vanilla", "furniture", "anchored"], actorCount: 1, genders: ["any"], requiresFurniture: true, sourceFile: "Data/OSF/vanilla/vanilla-furniture.osf.json", stages: [{ index: 0, name: "Idle", tags: [], clipCount: 1, loopSec: 2.7, loops: 0, openEnded: true, estSec: 5.4 }, { index: 1, name: "Idle_Flavor01", tags: ["transition"], clipCount: 1, loopSec: 11, loops: 0, openEnded: true, estSec: 22 }, { index: 2, name: "EnterFromStand", tags: ["transition", "rootmotion"], clipCount: 1, loopSec: 7.3, loops: 0, openEnded: true, estSec: 14.7 }], estSec: 42.1, estPartial: false, openEnded: true, priority: 0, weight: 1 },
  { id: "vanilla/furniture/bench", title: "Vanilla · Furniture / Bench", tags: ["vanilla", "furniture", "anchored"], actorCount: 1, genders: ["any"], requiresFurniture: true, sourceFile: "Data/OSF/vanilla/vanilla-furniture.osf.json", stages: [{ index: 0, name: "Idle", tags: [], clipCount: 1, loopSec: 3.1, loops: 0, openEnded: true, estSec: 6.2 }], estSec: 6.2, estPartial: false, openEnded: true, priority: 0, weight: 1 },
  { id: "vanilla/photomode", title: "Vanilla · Photomode", tags: ["vanilla", "photomode"], actorCount: 1, genders: ["any"], requiresFurniture: false, sourceFile: "Data/OSF/vanilla/vanilla-photomode.osf.json", stages: [{ index: 0, name: "Vehicle_BackSeat", tags: [], clipCount: 1, loopSec: 0.3, loops: 0, openEnded: true, estSec: 0.6 }, { index: 1, name: "Vehicle_HangTen", tags: [], clipCount: 1, loopSec: 0.3, loops: 0, openEnded: true, estSec: 0.6 }], estSec: 1.2, estPartial: false, openEnded: true, priority: 0, weight: 1 },
  { id: "vanilla/common", title: "Vanilla · Common", tags: ["vanilla", "common"], actorCount: 1, genders: ["any"], requiresFurniture: false, sourceFile: "Data/OSF/vanilla/vanilla-common.osf.json", stages: [{ index: 0, name: "Cower_Idle", tags: ["idle"], clipCount: 1, loopSec: 6.3, loops: 0, openEnded: true, estSec: 12.7 }], estSec: 12.7, estPartial: false, openEnded: true, priority: 0, weight: 1 },
];

function mockNative(command, fields) {
  if (command === "osf.catalog.get") setTimeout(() => handleCatalog(MOCK_CATALOG), 60);
  else if (command === "osf.library.get") setTimeout(() => handleLibrary(MOCK_LIBRARY), 90);
  else if (command === "osf.anchorMatch") setTimeout(() => handleAnchorMatch({ token: fields.token, sceneIds: MOCK_ANCHOR_MATCH[fields.token] || [] }), 70);
  else if (command === "osf.pickCrosshair") { const item = fields.slot === "furniture" ? MOCK_ANCHORS[0] : MOCK_ACTORS[0]; setTimeout(() => handlePick({ slot: fields.slot, valid: true, ...item }), 60); }
  else if (command === "osf.scanNearby") setTimeout(() => handleScanResults({ kind: fields.kind, items: fields.kind === "furniture" ? MOCK_ANCHORS : MOCK_ACTORS }), 80);
  else if (command === "osf.launch") setTimeout(() => handleLaunchResult({ ok: true, handle: 42, sceneId: fields.sceneId, stage: fields.opts && fields.opts.stage }), 80);
  else if (command === "osf.stop") setTimeout(() => notice("ok", "Scene stopped."), 40);
}

init();
