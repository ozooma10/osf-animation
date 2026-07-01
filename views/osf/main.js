// OSF Animation - Scene Director. NASA-punk maintenance-HUD console.
// Visual redesign of the in-game view (Claude Design "Scene Director"), wired to the
// unchanged OSF UI bridge contract: only JSON text crosses window.osfui.
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
  showAnim: false,
  opts: { strip: "-1", lock: "-1", camera: "", speed: "1" },
  filters: { search: "", tags: new Set(), authorMode: false },
  catalogReceived: false,
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

function onNativeMessage(jsonText) {
  let msg;
  try { msg = JSON.parse(jsonText); } catch { return; }
  const { type, payload } = msg;
  switch (type) {
    case "runtime.ready": handleReady(payload); break;
    case "osf.catalog.data": handleCatalog(payload); break;
    case "osf.pick": handlePick(payload); break;
    case "osf.scanResults": handleScanResults(payload); break;
    case "osf.launchResult": handleLaunchResult(payload); break;
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
  }));
  if (kind === "furniture") {
    state.nearbyFurniture = normalized;
    notice("info", `${normalized.length} usable anchor${normalized.length === 1 ? "" : "s"} found.`);
  } else {
    state.nearbyActors = normalized;
    notice("info", `${normalized.length} nearby actor${normalized.length === 1 ? "" : "s"} found.`);
  }
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
      sig: String(st.sig || ""),
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
      edges: Array.isArray(raw.shape.edges) ? raw.shape.edges : [],
    };
  }
  const stages = Number(raw.stageCount || raw.linearStageCount || 0);
  const nodes = Number(raw.nodeCount || 0);
  const branches = Number(raw.branchCount || 0);
  return { kind: branches > 0 || nodes > stages ? "graph" : "linear", stages: stages || (actorCount ? 1 : 0), nodes: nodes || (stages || 1), branches, edges: [] };
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
function applyPick(slot, token, name, distance) {
  if (slot === "furniture") {
    state.furniture = { token, name: name || "anchor", distance: distance || null };
    notice("info", `Anchor keyed: ${state.furniture.name}.`);
  } else if (token === PLAYER_TOKEN || state.partners.some((c) => c.token === token)) {
    notice("info", `${name || "Actor"} is already in the cast.`);
  } else {
    state.partners.push({ token, name: name || "actor", distance: distance || null });
    notice("info", `Cast added: ${name || "actor"}.`);
  }
  renderAll();
}

function toggleActor(token) {
  const idx = state.partners.findIndex((p) => p.token === token);
  if (idx >= 0) { state.partners.splice(idx, 1); notice("info", "Cast member removed."); }
  else {
    const a = state.nearbyActors.find((x) => x.token === token);
    if (a) { state.partners.push({ token: a.token, name: a.name, distance: a.distance }); notice("info", `Cast added: ${a.name}.`); }
  }
  renderAll();
}

function toggleAnchor(token) {
  if (state.furniture && state.furniture.token === token) { state.furniture = null; notice("info", "Anchor cleared."); }
  else {
    const an = state.nearbyFurniture.find((x) => x.token === token);
    if (an) { state.furniture = { token: an.token, name: an.name, distance: an.distance }; notice("info", `Anchor keyed: ${an.name}.`); }
  }
  renderAll();
}

function toggleTag(t) {
  if (state.filters.tags.has(t)) state.filters.tags.delete(t);
  else state.filters.tags.add(t);
  renderAll();
}

function removePartner(index) { state.partners.splice(index, 1); renderAll(); }

function scanNearby(kind) {
  notice("info", `Scanning nearby ${kind === "furniture" ? "anchors" : "actors"}…`);
  send("osf.scanNearby", { kind, sceneId: state.selectedId || "" });
}

function selectScene(id) { state.selectedId = id; state.showAnim = false; renderAll(); }
function sceneById(id) { return state.catalog.find((s) => s.id === id) || null; }
function sceneTitle(id) { const s = sceneById(id); return s ? s.title : (id || "scene"); }
function castTokens() { return [PLAYER_TOKEN, ...state.partners.map((c) => c.token)]; }
function castMembers() { return [PLAYER_CAST, ...state.partners]; }

function unlistedVisible(s) { return !!s && (!s.unlisted || state.filters.authorMode || state.allUnlisted); }

function ensureSelection() {
  // Track the fully-filtered pool (search + tags + unlisted) so the brief never
  // inspects a scene that no bay shows. Re-pick to the top visible scene if the
  // current selection is filtered out.
  const vis = state.catalog.filter(matchesFilters);
  if (state.selectedId && vis.some((s) => s.id === state.selectedId)) return;
  const best = bestInitialScene();
  if (best) { state.selectedId = best; return; }
  // Nothing matches the active filters — keep a valid selection so the brief isn't blank.
  if (!state.selectedId || !state.catalog.some((s) => s.id === state.selectedId)) {
    const fallback = state.catalog.find(unlistedVisible) || state.catalog[0];
    state.selectedId = fallback ? fallback.id : null;
  }
}

function bestInitialScene() {
  const g = groupedScenes();
  const first = g.ready[0] || g.need[0] || g.library[0];
  return first ? first.scene.id : null;
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
  const anchorGate = s.requiresFurniture ? !!state.furniture : true;
  const seated = hasRoles ? Math.min(castCount, actorCount) : 0;
  const issues = [];
  const blockers = [];
  // A scene with no fillable roles is never READY — it can't be seated or launched.
  if (!hasRoles) blockers.push("scene defines no roles");
  else if (!rolesGate) { const n = actorCount - castCount; issues.push(`needs ${n} more actor${n === 1 ? "" : "s"}`); }
  if (overCast) { const n = castCount - actorCount; blockers.push(`remove ${n} cast member${n === 1 ? "" : "s"}`); }
  if (!anchorGate) issues.push("needs an anchor");
  const gaps = issues.length + blockers.length;
  let cls, tone, label;
  if (gaps === 0) { cls = "ready"; tone = "ready"; label = "READY"; }
  else if (blockers.length) { cls = "library"; tone = "blocked"; label = "BLOCKED"; }
  else if (issues.length === 1) { cls = "need"; tone = "need"; label = "NEEDS ONE"; }
  else { cls = "library"; tone = "library"; label = "LIBRARY"; }
  const reason = gaps === 0 ? "Ready with the current cast and anchor." : [...issues, ...blockers].map(sentenceCase).join(". ") + ".";
  return { castCount, actorCount, hasRoles, rolesGate, overCast, anchorGate, seated, issues, blockers, gaps, cls, tone, label, reason };
}

function needsText(s, ev) {
  if (!ev.rolesGate) { const n = ev.actorCount - ev.castCount; return `needs ${n} more actor${n === 1 ? "" : "s"}`; }
  if (!ev.anchorGate) return "needs an anchor";
  if (ev.overCast) { const n = ev.castCount - ev.actorCount; return `remove ${n} cast member${n === 1 ? "" : "s"}`; }
  return "";
}

/* =========================================================================
   FILTER + GROUP
   ========================================================================= */
function matchesFilters(s) {
  const f = state.filters;
  if (!unlistedVisible(s)) return false;
  if (f.search) {
    const roleText = (s.roles || []).map((r) => `${r.name} ${r.gender}`).join(" ");
    const hay = `${s.title} ${s.id} ${(s.tags || []).join(" ")} ${roleText} ${s.sourceFile}`.toLowerCase();
    if (!hay.includes(f.search)) return false;
  }
  if (f.tags.size) {
    const st = new Set((s.tags || []).map((t) => t.toLowerCase()));
    let hit = false;
    for (const t of f.tags) if (st.has(t)) { hit = true; break; }
    if (!hit) return false;
  }
  return true;
}

function groupedScenes() {
  const pool = state.catalog.filter(matchesFilters);
  const ready = [], need = [], library = [];
  for (const scene of pool) {
    const ev = evalScene(scene);
    const item = { scene, ev, sel: scene.id === state.selectedId };
    if (ev.cls === "ready") ready.push(item);
    else if (ev.cls === "need") need.push(item);
    else library.push(item);
  }
  const byRank = (a, b) => b.scene.priority - a.scene.priority || b.scene.weight - a.scene.weight || a.scene.title.localeCompare(b.scene.title);
  ready.sort(byRank); need.sort(byRank); library.sort(byRank);
  return { ready, need, library };
}

function catalogTags() {
  const m = new Map();
  for (const s of state.catalog) {
    if (!unlistedVisible(s)) continue;
    for (const t of s.tags || []) { const k = t.toLowerCase(); m.set(k, (m.get(k) || 0) + 1); }
  }
  return [...m.keys()].sort();
}

/* =========================================================================
   RENDER
   ========================================================================= */
function renderAll() {
  ensureSelection();
  const groups = groupedScenes();
  renderSlate(groups);
  renderWorld();
  renderBays(groups);
  renderBrief();
}

/* ---- slate -------------------------------------------------------------- */
function renderSlate(groups) {
  $("readyCount").textContent = String(groups.ready.length);
  $("needCount").textContent = String(groups.need.length);
  $("libCount").textContent = String(groups.library.length);
  $("authorToggle").classList.toggle("on", state.filters.authorMode);
  $("slateCast").innerHTML = castMembers().map(castChip).join("");
  $("slateAnchor").innerHTML = anchorSlotHTML();
}

function castChip(m, i) {
  const key = String.fromCharCode(65 + i);
  const player = m.kind === "player";
  const sub = player ? "PLAYER" : (m.distance ? `${Math.max(1, Math.round(m.distance))}M` : "CAST");
  const drop = player ? "" : `<button class="cast-drop" data-act="drop" data-i="${i - 1}" title="Remove from cast">×</button>`;
  return `<span class="cast-chip"><span class="cast-key">${key}</span><div style="min-width:0"><div class="cast-name">${esc(m.name)}</div><div class="cast-sub">${esc(sub)}</div></div>${drop}</span>`;
}

function anchorSlotHTML() {
  if (state.furniture) {
    const d = state.furniture.distance ? ` ${Math.max(1, Math.round(state.furniture.distance))}m` : "";
    return `<div class="anchor-slot keyed"><span class="dot" style="background:var(--signal-go);box-shadow:0 0 7px var(--signal-go)"></span><span class="anchor-name">${esc(state.furniture.name)}</span><span class="anchor-state">matched${d}</span><button class="anchor-chg" data-act="clear-anchor" title="Clear anchor">CLR▸</button></div>`;
  }
  return `<div class="anchor-slot"><span class="dot" style="background:var(--steel-500)"></span><span class="anchor-name">No anchor</span><span class="anchor-state">free-space</span></div>`;
}

/* ---- world -------------------------------------------------------------- */
function renderWorld() {
  $("worldBody").innerHTML = worldActors() + worldAnchors() + worldTags() + worldFoot();
}

function worldActors() {
  const rows = state.nearbyActors.length
    ? state.nearbyActors.map((a) => {
        const added = state.partners.some((p) => p.token === a.token);
        const meta = `${a.distance != null ? Math.max(1, Math.round(a.distance)) + "m · " : ""}actor`;
        return `<button class="pick-row ${added ? "active" : ""}" data-act="toggle-actor" data-token="${a.token}"><span class="pick-av"></span><div class="pick-body"><span class="val">${esc(a.name)}</span><span class="mono">${esc(meta)}</span></div><span class="pick-tag ${added ? "added" : ""}">${added ? "ADDED" : "ADD▸"}</span></button>`;
      }).join("")
    : `<div class="empty-mini"><span class="mono">No actors in range. Scan, or move near someone.</span></div>`;
  return `<div class="world-group"><div class="world-head"><p class="lbl">◐ NEARBY ACTORS · ${state.nearbyActors.length}</p><div class="world-tools"><button class="chip-btn" data-act="scan" data-kind="actor">SCAN</button><button class="chip-btn" data-act="pick" data-slot="actor">PICK</button></div></div><div class="world-list">${rows}</div></div>`;
}

function worldAnchors() {
  const rows = state.nearbyFurniture.length
    ? state.nearbyFurniture.map((an) => {
        const active = state.furniture && state.furniture.token === an.token;
        const meta = `${an.distance != null ? Math.max(1, Math.round(an.distance)) + "m · " : ""}anchor`;
        return `<button class="pick-row ${active ? "active" : ""}" data-act="toggle-anchor" data-token="${an.token}"><span class="pick-av"></span><div class="pick-body"><span class="val">${esc(an.name)}</span><span class="mono">${esc(meta)}</span></div><span class="pick-tag ${active ? "active" : ""}">${active ? "ACTIVE" : "USE▸"}</span></button>`;
      }).join("")
    : `<div class="empty-mini"><span class="mono">No anchor in range — free-space scenes only.</span></div>`;
  return `<div class="world-group"><div class="world-head"><p class="lbl">◫ NEARBY ANCHORS · ${state.nearbyFurniture.length}</p><div class="world-tools"><button class="chip-btn" data-act="scan" data-kind="furniture">SCAN</button><button class="chip-btn" data-act="pick" data-slot="furniture">PICK</button></div></div><div class="world-list">${rows}</div></div>`;
}

function worldTags() {
  const tags = catalogTags();
  const chips = tags.length
    ? tags.map((t) => `<button class="tag ${state.filters.tags.has(t) ? "on" : ""}" data-act="toggle-tag" data-tag="${escAttr(t)}">${esc(t)}</button>`).join("")
    : `<span class="mono">no tags</span>`;
  return `<div class="world-group"><div class="world-head"><p class="lbl">⊞ TAGS · ${tags.length}</p></div><div class="tag-rail">${chips}</div></div>`;
}

function worldFoot() {
  const active = state.filters.search || state.filters.tags.size;
  if (active) {
    const bits = [];
    if (state.filters.search) bits.push(`“${state.filters.search}”`);
    if (state.filters.tags.size) bits.push("tags " + [...state.filters.tags].join("+"));
    return `<div class="world-foot"><button class="focus-clear" data-act="clear-filters"><span class="mono">Filtered by ${esc(bits.join(" + "))} — tap to clear</span></button></div>`;
  }
  return `<div class="world-foot"><div class="focus-hint"><span class="mono">Scan the world, add actors to the cast, key an anchor — the bays re-rank as readiness changes.</span></div></div>`;
}

/* ---- bays --------------------------------------------------------------- */
function renderBays(groups) {
  const { ready, need, library } = groups;
  const readyBody = ready.length ? `<div class="scene-grid">${ready.map((i) => cardHTML(i, "ready")).join("")}</div>` : `<div class="scene-grid">${bayEmpty("Nothing seats with the current cast. Check NEEDS ONE THING below.")}</div>`;
  const needBody = need.length ? `<div class="scene-grid">${need.map((i) => cardHTML(i, "need")).join("")}</div>` : `<div class="scene-grid">${bayEmpty("No near-misses right now.")}</div>`;
  const libBody = library.length ? `<div class="library-strip">${library.map(libHTML).join("")}</div>` : `<div class="scene-grid">${bayEmpty("Everything else appears here as your cast changes.")}</div>`;
  $("bays").innerHTML =
    `<div>${bayHead("go", "BAY 1 · READY NOW", ready.length, "seats with current cast")}${readyBody}</div>` +
    `<div>${bayHead("warn", "BAY 2 · NEEDS ONE THING", need.length, "one gap to seat")}${needBody}</div>` +
    `<div>${bayHead("lib", "BAY 3 · LIBRARY", library.length, "")}${libBody}</div>`;
}

function bayHead(tone, label, count, note) {
  const color = tone === "go" ? "var(--signal-go)" : tone === "warn" ? "var(--signal-warn)" : "var(--steel-500)";
  const glow = tone === "lib" ? "" : `box-shadow:0 0 9px ${color}`;
  const noteHTML = note ? `<span class="bay-note">${esc(note)}</span>` : "";
  return `<div class="bay-head"><span class="dot" style="width:9px;height:9px;background:${color};${glow}"></span><p class="eb ${tone}">${esc(label)} · ${count}</p><span class="bay-rule ${tone}"></span>${noteHTML}</div>`;
}

function bayEmpty(msg) { return `<div class="bay-empty"><span class="mono">${esc(msg)}</span></div>`; }

function cardHTML(item, cls) {
  const { scene: s, ev, sel } = item;
  const pips =
    pipHTML(ev.rolesGate && !ev.overCast, `ROLES ${ev.actorCount ? ev.seated + "/" + ev.actorCount : "0"}`) +
    pipHTML(ev.anchorGate, s.requiresFurniture ? "ANCHOR" : "FREE") +
    pipHTML(true, contentPip(s));
  const needs = cls === "need" ? `<div class="needs-text">${esc(needsText(s, ev))}</div>` : "";
  const author = state.filters.authorMode
    ? `<div class="card-author"><div class="mono wrap a-id">${esc(s.id)} · w${s.weight} p${s.priority}</div><div class="mono wrap a-src">${esc(s.sourceFile || "live registry")}</div></div>`
    : "";
  const dur = fmtEst(s);
  const durHTML = dur ? `<span class="card-dur">${esc(dur)}</span>` : "";
  return `<div class="scene-card ${cls} ${sel ? "selected" : ""}" data-act="select-scene" data-id="${escAttr(s.id)}"><div class="card-spine"><span class="card-num">${displayNum(s)}</span></div><div class="card-body"><div class="card-nameline"><span class="card-name">${esc(s.title)}</span>${durHTML}<span class="card-lamp ${cls === "need" ? "warn" : ""}"></span></div><div class="pip-row ${cls === "need" ? "need" : ""}">${pips}</div>${needs}${author}</div></div>`;
}

function pipHTML(pass, label) {
  return `<span class="pip ${pass ? "pass" : "fail"}"><span class="pip-dot"></span><span class="pip-label">${esc(label)}</span></span>`;
}

function libHTML(item) {
  const { scene: s, sel } = item;
  const n = s.actorCount || (s.roles || []).length || 1;
  const dur = fmtEst(s);
  const meta = state.filters.authorMode ? s.id : `${n} role${n === 1 ? "" : "s"}${dur ? ` · ${dur}` : ""}`;
  return `<div class="lib-chip ${sel ? "selected" : ""}" data-act="select-scene" data-id="${escAttr(s.id)}"><span class="lib-spine"></span><div class="lib-body"><div class="val">${esc(s.title)}</div><div class="mono">${esc(meta)}</div></div></div>`;
}

/* ---- brief -------------------------------------------------------------- */
function renderBrief() {
  const brief = $("brief");
  const s = sceneById(state.selectedId);
  if (!s) { brief.innerHTML = `<div class="brief-empty"><span class="mono">No scene selected. The catalog is empty.</span></div>`; return; }
  const ev = evalScene(s);
  const allMet = ev.gaps === 0;

  const statusRow = `<div class="brief-status ${allMet ? "" : "warn"}"><span class="dot"></span><p class="eb">${allMet ? "SEATED IN BRIEF" : "INSPECTING · NOT SEATABLE"}</p></div>`;

  const reqRows =
    reqRow("ROLES", ev.rolesGate && !ev.overCast, ev.actorCount ? `${ev.seated}/${ev.actorCount}${ev.overCast ? " · over-cast" : ev.rolesGate ? " seated" : " short"}` : "no roles defined") +
    reqRow("ANCHOR", ev.anchorGate, s.requiresFurniture ? (ev.anchorGate ? `${state.furniture.name} keyed` : "anchor required") : "free-space") +
    reqRow("CONTENT", true, contentDetail(s));

  const roleCount = Math.max(ev.actorCount || 0, (s.roles || []).length);
  const members = castMembers();
  let seatsHTML = "";
  for (let i = 0; i < roleCount; i++) {
    const role = (s.roles && s.roles[i]) || { name: `role ${i + 1}`, gender: "any" };
    const m = members[i];
    const key = String.fromCharCode(65 + i);
    const name = m ? m.name : "— open —";
    const sub = m ? "SEATED" : `NEEDS ${(role.gender || "any").toUpperCase()}`;
    seatsHTML += `<div class="seat ${m ? "" : "open"}"><span class="seat-key">${key}</span><div style="min-width:0"><div class="seat-name">${esc(name)}</div><div class="seat-sub">${esc(sub)}</div></div></div>`;
  }
  const anchorChip = `<div class="anchor-mini"><span>◫ ${esc(s.requiresFurniture ? (state.furniture ? state.furniture.name : "anchor req") : "no anchor")}</span></div>`;

  const module = `<div class="module ${allMet ? "seated" : ""}"><div class="module-spine"><span class="rivet"></span><span class="snum">SCN-${displayNum(s)}</span><span class="rivet"></span></div><div class="module-body"><div class="module-top"><div class="registry"><div class="r-label">REGISTRY</div><div class="r-id">${esc(s.id)}</div></div><div class="status-gauge"><div class="gauge-ring"><span class="gauge-lamp ${allMet ? "" : "warn"}"></span></div><span class="gauge-label ${allMet ? "" : "warn"}">${allMet ? "READY" : ev.gaps + " GAP"}</span></div></div><div class="module-name">${esc(s.title)}</div><div class="req-box"><div class="req-head"><span class="r-label">REQUIREMENTS</span><span class="req-met ${allMet ? "" : "warn"}">${allMet ? "ALL MET" : ev.gaps + " OPEN"}</span></div><div class="req-list">${reqRows}</div></div><div class="seat-row">${seatsHTML}${anchorChip}</div><div class="module-edge"><div class="edge-bar ${allMet ? "lit" : ""}"></div></div></div></div>`;

  const live = state.lastHandle ? liveTakeHTML() : "";

  const shapeBox = `<div class="info-box"><div class="lbl">SHAPE</div><div class="mono wrap">${esc(shapeText(s))}</div></div>`;

  const diagBox = state.filters.authorMode
    ? `<div class="info-box hud"><div class="lbl">MATCH DIAGNOSTICS</div><div class="kv-list">${diagRows(s, ev).map(([k, v]) => `<div class="kv"><span class="k">${esc(k)}</span><span class="v">${esc(v)}</span></div>`).join("")}</div></div>`
    : "";

  const polBox = `<div class="info-box"><div class="lbl">POLICIES</div><div class="policy-list">${policyLines(s).map((p) => `<div class="mono wrap"><b>●</b> ${esc(p)}</div>`).join("")}</div></div>`;

  const o = state.opts;
  const overrides = `<div class="info-box"><div class="lbl">START OVERRIDES</div><div class="override-grid"><label class="override"><span class="lbl">STRIP</span><select class="select" data-field="strip">${optionTags([["-1", "Inherit"], ["1", "On"], ["0", "Off"]], o.strip)}</select></label><label class="override"><span class="lbl">LOCK PLAYER</span><select class="select" data-field="lock">${optionTags([["-1", "Inherit"], ["1", "On"], ["0", "Off"]], o.lock)}</select></label><label class="override"><span class="lbl">CAMERA</span><select class="select" data-field="camera">${optionTags([["", "Inherit"], ["thirdperson_hold", "Third person"], ["scene_orbit", "Scene orbit"], ["freefly", "Free fly"], ["vanity_orbit", "Vanity orbit"]], o.camera)}</select></label><label class="override"><span class="lbl">SPEED <b id="speedVal">${Number(o.speed).toFixed(1)}x</b></span><input id="optSpeed" class="range" type="range" min="0.1" max="3" step="0.1" value="${escAttr(o.speed)}"></label></div></div>`;

  const stages = s.stages || [];
  const canPlay = state.ready && ev.gaps === 0;
  const animBox = state.showAnim && stages.length
    ? `<div class="info-box"><div class="lbl">ANIMATIONS · ${stages.length}</div><div class="anim-list">${stages.map((st) => {
        const label = st.name || `Stage ${st.index}`;
        const tags = (st.tags || []).slice(0, 3).map((t) => `<span class="pill">${esc(t)}</span>`).join("");
        const loop = fmtDur(st.loopSec);
        const dur = loop || fmtDur(st.estSec);  // no loop length yet: fall back to the stage estimate (e.g. a timer)
        const durHTML = dur ? `<span class="anim-dur" title="${loop ? "Loop length" : "Stage time"}">${esc(dur)}${st.openEnded ? "∞" : ""}</span>` : "";
        return `<div class="anim-row"><div class="anim-main"><span class="anim-name">${esc(label)}</span><div class="anim-tags">${tags}</div></div>${durHTML}<button class="anim-play" data-act="play-stage" data-stage="${st.index}" ${canPlay ? "" : "disabled"} title="Play this animation">▶</button></div>`;
      }).join("")}</div></div>`
    : "";

  const reason = launchReason(s, ev);
  const launchBtn = canPlay
    ? `<button class="launch-btn go" data-act="launch">▶ Launch Scene</button>`
    : `<button class="launch-btn blocked" data-act="launch-blocked" disabled>${esc(canPlay ? "" : launchLabel(s, ev))}</button>`;
  const previewBtn = stages.length ? `<button class="ghost-btn ${state.showAnim ? "on" : ""}" data-act="toggle-anim">${state.showAnim ? "Hide Shape" : "Preview Shape"}</button>` : "";
  const stopBtn = state.lastHandle ? `<button class="stop-btn" data-act="stop">■ Stop Take #${state.lastHandle}</button>` : "";
  const reasonHTML = reason ? `<div class="mono wrap" style="color:var(--text-faint);text-align:center">${esc(reason)}</div>` : "";
  const launchStack = `<div class="launch-stack">${reasonHTML}${launchBtn}${previewBtn}${stopBtn}</div>`;

  brief.innerHTML = statusRow + module + live + shapeBox + diagBox + polBox + overrides + animBox + launchStack;
}

function reqRow(label, pass, detail) {
  return `<span class="req-row ${pass ? "" : "fail"}"><span class="rdot"></span><span class="rlabel">${esc(label)}</span><span class="req-fill"></span><span class="rdetail">${esc(detail)}</span></span>`;
}

function liveTakeHTML() {
  const ls = sceneById(state.lastSceneId) || sceneById(state.selectedId);
  const kind = ls ? shapeText(ls).split(" · ")[0] : "runtime";
  return `<div class="live-take"><div class="live-head"><span class="live-dot"></span><div class="live-body"><span class="lbl">LIVE TAKE</span><strong>${esc(ls ? ls.title : state.lastSceneId)}</strong></div><b>#${state.lastHandle}</b></div><div class="live-grid"><span>Cast ${castTokens().length}</span><span>${esc(kind)}</span><span>${state.furniture ? "Anchored" : "Free"}</span></div></div>`;
}

function launchLabel(s, ev) {
  if (!state.ready) return "Engine Offline";
  return `Launch Blocked · ${ev.gaps} gap${ev.gaps > 1 ? "s" : ""}`;
}

function launchReason(s, ev) {
  if (!state.ready) return "Engine not connected.";
  if (ev.gaps > 0) return ev.reason;
  return "";
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

/* ---- brief value helpers ------------------------------------------------ */
function contentPip(s) {
  const n = (s.stages || []).length;
  if (n) return `${n} STG`;
  return String((s.shape && s.shape.kind) || "linear").toUpperCase();
}

function contentDetail(s) {
  const stages = s.stages || [];
  if (stages.length) {
    const clips = stages.reduce((a, st) => a + (st.clipCount || 0), 0);
    return `${stages.length} stage${stages.length === 1 ? "" : "s"}${clips ? ` · ${clips} clips` : ""}`;
  }
  return "clips resolved";
}

function shapeText(s) {
  const sh = s.shape || {};
  const kind = String(sh.kind || "linear");
  const graph = kind === "graph";
  const n = graph ? (sh.nodes || 1) : (sh.stages || 1);
  const unit = graph ? "node" : "stage";
  const parts = [kind, `${n} ${unit}${n === 1 ? "" : "s"}`];
  if (sh.branches) parts.push(`${sh.branches} branch${sh.branches === 1 ? "" : "es"}`);
  const dur = fmtEst(s);
  if (dur) parts.push(dur);
  return parts.join(" · ");
}

function policyLines(s) {
  const p = s.policy || {};
  const out = [
    `strip actors: ${p.stripActors}`,
    `lock player: ${p.lockPlayer}`,
    `fade: ${p.fade}`,
    `camera: ${p.camera || "inherit"}`,
  ];
  if (p.playerControl) out.push("player control: custom");
  return out;
}

function diagRows(s, ev) {
  const rows = [["weight · priority", `${s.weight} · ${s.priority}`]];
  (s.roles || []).forEach((r, i) => rows.push([`role ${String.fromCharCode(65 + i)} filter`, `${r.gender || "any"} · ${ev.seated > i ? "pass" : "open"}`]));
  rows.push(["anchor", (s.requiresFurniture ? "required" : "free-space") + " · " + (ev.anchorGate ? "pass" : "fail")]);
  rows.push(["stages · shape", `${(s.stages || []).length} · ${(s.shape && s.shape.kind) || "linear"}`]);
  rows.push(["est duration", fmtEst(s) || "unmeasured"]);
  rows.push(["source", s.sourceFile || "live registry"]);
  return rows;
}

function optionTags(pairs, val) {
  return pairs.map(([v, l]) => `<option value="${escAttr(v)}" ${String(v) === String(val) ? "selected" : ""}>${esc(l)}</option>`).join("");
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
function displayNum(s) {
  const id = s.id || s.title || "";
  let h = 0;
  for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) >>> 0;
  return String(h % 1000).padStart(3, "0");
}

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
    case "toggle-actor": toggleActor(Number(el.dataset.token)); break;
    case "toggle-anchor": toggleAnchor(Number(el.dataset.token)); break;
    case "toggle-tag": toggleTag(el.dataset.tag); break;
    case "scan": scanNearby(el.dataset.kind); break;
    case "pick": send("osf.pickCrosshair", { slot: el.dataset.slot }); break;
    case "clear-filters": state.filters.search = ""; $("search").value = ""; state.filters.tags.clear(); renderAll(); break;
    case "clear-anchor": state.furniture = null; renderAll(); break;
    case "drop": removePartner(Number(el.dataset.i)); break;
    case "toggle-anim": state.showAnim = !state.showAnim; renderBrief(); break;
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
  document.addEventListener("click", onClick);
  document.addEventListener("change", onChange);
  document.addEventListener("input", onInput);
  $("refresh").addEventListener("click", () => { notice("info", "Refreshing catalog…"); requestCatalog(true); });
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
  { token: 501, name: "Industrial Chair", formId: 0x12a57a, distance: 3, isActor: false },
  { token: 502, name: "Ak Bunk Bed", formId: 0x1234, distance: 6, isActor: false },
  { token: 503, name: "Crew Quarters Marker", formId: 0x2234, distance: 11, isActor: false },
];

function mockNative(command, fields) {
  if (command === "osf.catalog.get") setTimeout(() => handleCatalog(MOCK_CATALOG), 60);
  else if (command === "osf.pickCrosshair") { const item = fields.slot === "furniture" ? MOCK_ANCHORS[0] : MOCK_ACTORS[0]; setTimeout(() => handlePick({ slot: fields.slot, valid: true, ...item }), 60); }
  else if (command === "osf.scanNearby") setTimeout(() => handleScanResults({ kind: fields.kind, items: fields.kind === "furniture" ? MOCK_ANCHORS : MOCK_ACTORS }), 80);
  else if (command === "osf.launch") setTimeout(() => handleLaunchResult({ ok: true, handle: 42, sceneId: fields.sceneId, stage: fields.opts && fields.opts.stage }), 80);
  else if (command === "osf.stop") setTimeout(() => notice("ok", "Scene stopped."), 40);
}

init();
