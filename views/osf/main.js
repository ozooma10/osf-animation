// OSF Animation - Scene Director. Keeps the existing OSF UI bridge contract.
"use strict";

const BRIDGE_PROTOCOL = "0.1";
const PLAYER_TOKEN = -1;
const PLAYER_CAST = { token: PLAYER_TOKEN, name: "You", kind: "player" };

const state = {
  ready: false,
  catalog: [],
  selectedId: null,
  partners: [],
  furniture: null,
  nearbyActors: [],
  nearbyFurniture: [],
  lastHandle: 0,
  lastSceneId: "",
  filters: { search: "", view: "all", count: "any", anchor: "any", showUnlisted: false, tags: new Set(), authorMode: false },
  catalogReceived: false,
};

const $ = (id) => document.getElementById(id);

function bridgeAvailable() {
  return typeof window.osfui === "object" && typeof window.osfui.postMessage === "function";
}

function send(command, fields = {}) {
  const msg = JSON.stringify({ type: "ui.command", payload: { command, ...fields } });
  if (bridgeAvailable()) window.osfui.postMessage(msg);
  else {
    console.log("(standalone) ->", msg);
    mockNative(command, fields);
  }
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
    $("statusText").textContent = `UNSUPPORTED BRIDGE ${bv || "?"}`;
    notice("err", `This view needs bridge protocol ${BRIDGE_PROTOCOL}; runtime reports ${bv || "?"}.`);
    return;
  }
  state.ready = true;
  setLamp("ok");
  $("statusText").textContent = `${(p.plugin || "OSF UI").toUpperCase()} v${p.version || "?"}`;
  notice("ok", `Bridge online. Protocol ${bv}.`);
  requestCatalog(true);
}

function handleCatalog(list) {
  state.catalogReceived = true;
  clearTimeout(catalogTimer);
  state.catalog = Array.isArray(list) ? list.map(normalizeScene) : [];
  $("sceneCount").textContent = `${state.catalog.length} SCENES`;
  syncUnlistedFilter();
  buildTagCloud();
  if (state.selectedId && !state.catalog.some((s) => s.id === state.selectedId)) state.selectedId = null;
  if (!state.selectedId && state.catalog.length) state.selectedId = bestInitialScene();
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
  };
}

// Each stage is a browsable animation: { index, name, tags, clipCount }. The native de-dup `sig` is
// kept so identical clip-sets could be collapsed later; the UI shows every stage in order for now.
function normalizeStages(stages) {
  if (!Array.isArray(stages)) return [];
  return stages.map((st, i) => ({
    index: Number.isInteger(st.index) ? st.index : i,
    name: String(st.name || ""),
    tags: Array.isArray(st.tags) ? st.tags.map((t) => String(t)) : [],
    clipCount: Number(st.clipCount || 0),
    sig: String(st.sig || ""),
  }));
}

function clampCount(actorCount, roles) {
  if (Number.isFinite(Number(actorCount)) && Number(actorCount) > 0) return Number(actorCount);
  if (Array.isArray(roles) && roles.length) return roles.length;
  return 0;
}

function normalizeRoles(roles, genders, actorCount) {
  if (Array.isArray(roles) && roles.length) {
    return roles.map((role, index) => ({
      name: String(role.name || `role ${index + 1}`),
      gender: String(role.gender || (role.filters && role.filters.gender) || "any"),
      filters: role.filters || {},
      equip: !!role.equip,
    }));
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

function applyPick(slot, token, name, distance) {
  if (slot === "furniture") {
    state.furniture = { token, name: name || "anchor", distance: distance || null };
    notice("info", `Anchor selected: ${state.furniture.name}.`);
  } else if (token === PLAYER_TOKEN || state.partners.some((c) => c.token === token)) {
    notice("info", `${name || "Actor"} is already in the cast.`);
  } else {
    state.partners.push({ token, name: name || "actor", distance: distance || null });
    notice("info", `Cast added: ${name || "actor"}.`);
  }
  renderAll();
}

function removePartner(index) {
  state.partners.splice(index, 1);
  renderAll();
}

function scanNearby(kind) {
  notice("info", `Scanning nearby ${kind === "furniture" ? "anchors" : "actors"}...`);
  send("osf.scanNearby", { kind, sceneId: state.selectedId || "" });
}

function selectScene(id) { state.selectedId = id; renderAll(); }
function sceneById(id) { return state.catalog.find((s) => s.id === id) || null; }
function sceneTitle(id) { const s = sceneById(id); return s ? s.title : (id || "scene"); }
function castTokens() { return [PLAYER_TOKEN, ...state.partners.map((c) => c.token)]; }
function castMembers() { return [PLAYER_CAST, ...state.partners]; }

function bestInitialScene() {
  const visible = state.catalog.filter(unlistedVisible);
  const ready = visible.find((s) => sceneFit(s).state === "ready");
  return (ready || visible[0] || state.catalog[0]).id;
}

function sceneFit(s) {
  const castCount = castTokens().length;
  const issues = [];
  const blockers = [];
  const actorCount = s.actorCount || 0;
  if (actorCount > 0 && castCount < actorCount) issues.push(`needs ${actorCount - castCount} actor${actorCount - castCount === 1 ? "" : "s"}`);
  if (actorCount > 0 && castCount > actorCount) blockers.push(`remove ${castCount - actorCount} cast`);
  if (s.requiresFurniture && !state.furniture) issues.push("needs anchor");
  if (!issues.length && !blockers.length) return { state: "ready", label: "READY", tone: "ready", reason: "Ready with current context." };
  if (!blockers.length && issues.length === 1) return { state: "need", label: "NEEDS ONE", tone: "need", reason: sentenceCase(issues[0]) + "." };
  return { state: "library", label: blockers.length ? "BLOCKED" : "LIBRARY", tone: blockers.length ? "blocked" : "library", reason: [...issues, ...blockers].map(sentenceCase).join(". ") + "." };
}

function unlistedVisible(s) { return !!s && (state.filters.showUnlisted || !s.unlisted); }

function matchesFilters(s) {
  const f = state.filters;
  if (!unlistedVisible(s)) return false;
  if (f.search) {
    const roleText = (s.roles || []).map((r) => `${r.name} ${r.gender}`).join(" ");
    const hay = `${s.title} ${s.id} ${(s.tags || []).join(" ")} ${roleText} ${s.sourceFile}`.toLowerCase();
    if (!hay.includes(f.search)) return false;
  }
  if (f.count !== "any") {
    const n = s.actorCount || 0;
    if (f.count === "3" ? n < 3 : n !== Number(f.count)) return false;
  }
  if (f.anchor === "yes" && !s.requiresFurniture) return false;
  if (f.anchor === "no" && s.requiresFurniture) return false;
  if (f.tags.size) {
    const st = new Set((s.tags || []).map((t) => t.toLowerCase()));
    let hit = false;
    for (const t of f.tags) if (st.has(t)) { hit = true; break; }
    if (!hit) return false;
  }
  const fit = sceneFit(s);
  if (f.view !== "all" && fit.state !== f.view) return false;
  return true;
}

function groupedScenes() {
  const all = state.catalog.filter(matchesFilters);
  const byTitle = (a, b) => a.title.localeCompare(b.title) || a.id.localeCompare(b.id);
  return {
    all,
    ready: all.filter((s) => sceneFit(s).state === "ready").sort(byTitle),
    need: all.filter((s) => sceneFit(s).state === "need").sort(byTitle),
    library: all.filter((s) => sceneFit(s).state === "library").sort(byTitle),
  };
}

function renderAll() {
  renderContext();
  renderWorld();
  renderShelves();
  renderBrief();
  renderLiveDirector();
  updateLaunch();
}

function renderContext() {
  const members = castMembers();
  $("castMetric").textContent = `${members.length} SELECTED`;
  $("contextCast").innerHTML = members.map(contextCastPill).join("");
  $("anchorMetric").textContent = state.furniture ? "SELECTED" : "NONE";
  $("contextAnchor").innerHTML = state.furniture ? `<span class="anchor-name">${esc(state.furniture.name)}</span>${distanceText(state.furniture)}` : `<span class="slot-empty">No anchor selected</span>`;
  $("clearFurniture").classList.toggle("hidden", !state.furniture);
}

function contextCastPill(member, index) {
  const role = index === 0 ? "PLAYER" : `ACTOR ${index}`;
  const drop = index === 0 ? "" : `<button class="pill-drop" type="button" data-drop="${index - 1}">x</button>`;
  return `<span class="cast-pill ${index === 0 ? "is-player" : ""}"><span>${role}</span><b>${esc(member.name)}</b>${drop}</span>`;
}

function renderWorld() {
  renderActorTokens();
  renderAnchorTokens();
  wireDynamicButtons();
}

function renderActorTokens() {
  const list = $("actorTokens");
  if (!state.nearbyActors.length) { list.innerHTML = `<div class="empty-mini">Scan nearby actors to build the cast.</div>`; return; }
  list.innerHTML = state.nearbyActors.map((actor) => {
    const added = state.partners.some((p) => p.token === actor.token);
    return `<button class="world-token ${added ? "is-added" : ""}" data-add-actor="${actor.token}" type="button"><span class="token-name">${esc(actor.name)}</span><span class="token-meta">${added ? "ADDED" : distanceLabel(actor)}</span></button>`;
  }).join("");
}

function renderAnchorTokens() {
  const list = $("anchorTokens");
  if (!state.nearbyFurniture.length) { list.innerHTML = `<div class="empty-mini">Scan nearby anchors for furniture-bound scenes.</div>`; return; }
  list.innerHTML = state.nearbyFurniture.map((anchor) => {
    const selected = state.furniture && state.furniture.token === anchor.token;
    return `<button class="world-token ${selected ? "is-added" : ""}" data-add-anchor="${anchor.token}" type="button"><span class="token-name">${esc(anchor.name)}</span><span class="token-meta">${selected ? "SELECTED" : distanceLabel(anchor)}</span></button>`;
  }).join("");
}

function wireDynamicButtons() {
  document.querySelectorAll("[data-drop]").forEach((btn) => { btn.onclick = (e) => { e.stopPropagation(); removePartner(Number(btn.dataset.drop)); }; });
  document.querySelectorAll("[data-add-actor]").forEach((btn) => { btn.onclick = () => { const actor = state.nearbyActors.find((a) => a.token === Number(btn.dataset.addActor)); if (actor) applyPick("actor", actor.token, actor.name, actor.distance); }; });
  document.querySelectorAll("[data-add-anchor]").forEach((btn) => { btn.onclick = () => { const anchor = state.nearbyFurniture.find((a) => a.token === Number(btn.dataset.addAnchor)); if (anchor) applyPick("furniture", anchor.token, anchor.name, anchor.distance); }; });
}

function renderShelves() {
  const groups = groupedScenes();
  const allVisible = groups.all;
  $("resultCount").textContent = `${allVisible.length} MATCHES`;
  $("readyCount").textContent = String(groups.ready.length);
  $("needsCount").textContent = String(groups.need.length);
  $("libraryCount").textContent = String(groups.library.length);
  $("empty").classList.toggle("hidden", allVisible.length > 0);
  const shelves = [];
  if (state.filters.view === "all" || state.filters.view === "ready") shelves.push(renderShelf("READY NOW", "Scenes that can launch with this cast and anchor.", groups.ready));
  if (state.filters.view === "all" || state.filters.view === "need") shelves.push(renderShelf("NEEDS ONE THING", "Close matches. Fill the missing slot and they become ready.", groups.need));
  if (state.filters.view === "all" || state.filters.view === "library") shelves.push(renderShelf("LIBRARY", "Everything else matching your filters.", groups.library));
  $("shelves").innerHTML = shelves.join("");
  document.querySelectorAll("[data-scene-id]").forEach((card) => { card.onclick = () => selectScene(card.dataset.sceneId); });
}

function renderShelf(title, note, items) {
  if (!items.length) return "";
  return `<section class="shelf"><div class="shelf-head"><div><h2>${esc(title)}</h2><p>${esc(note)}</p></div><span>${items.length}</span></div><div class="scene-strip">${items.map(renderSceneCard).join("")}</div></section>`;
}

function renderSceneCard(s) {
  const fit = sceneFit(s);
  return `<article class="scene-card ${s.id === state.selectedId ? "is-selected" : ""} ${fit.tone}" data-scene-id="${escAttr(s.id)}" role="listitem"><div class="card-topline"><span class="fit-badge ${fit.tone}">${esc(fit.label)}</span><span>${esc(shapeLabel(s))}</span></div><h3>${esc(s.title)}</h3><div class="scene-id ${state.filters.authorMode ? "" : "soft"}">${esc(s.id)}</div><div class="role-diagram">${esc(roleLine(s))}</div>${timelinePips(s)}<div class="card-tags">${s.tags.slice(0, 5).map((t) => `<span class="pill">${esc(t)}</span>`).join("")}</div><div class="card-foot"><span>${s.actorCount || "?"} CAST</span><span>${s.requiresFurniture ? "ANCHOR" : "FREE"}</span>${s.unlisted ? "<span>UNLISTED</span>" : ""}</div></article>`;
}

function renderBrief() {
  const s = sceneById(state.selectedId);
  const brief = $("brief");
  if (!s) { brief.innerHTML = `<div class="brief-empty">Select a scene slate.</div>`; return; }
  const fit = sceneFit(s);
  brief.innerHTML = `<div class="brief-head"><span class="fit-badge ${fit.tone}">${esc(fit.label)}</span><h2>${esc(s.title)}</h2><div class="brief-id">${esc(s.id)}</div></div><div class="brief-section"><div class="section-label">ROLE SOCKETS</div><div class="role-sockets">${renderRoleSockets(s)}</div></div><div class="brief-section"><div class="section-label">ANCHOR</div>${renderAnchorRequirement(s)}</div><div class="brief-section"><div class="section-label">SHAPE</div><div class="shape-readout"><span>${esc(shapeLabel(s))}</span>${timelinePips(s)}</div></div>${renderAnimations(s, fit)}<div class="brief-section"><div class="section-label">TAGS</div><div class="brief-tags">${s.tags.map((t) => `<span class="pill">${esc(t)}</span>`).join("") || "<span class='muted'>none</span>"}</div></div>${state.filters.authorMode ? renderAuthorBlock(s) : ""}<div class="brief-reason ${fit.tone}">${esc(fit.reason)}</div>`;
  brief.querySelectorAll("[data-play-stage]").forEach((btn) => { btn.onclick = () => doLaunch(Number(btn.dataset.playStage)); });
}

// Each stage is an individually browsable/playable animation. Listed in order; the play button enters
// the scene directly on that stage (SceneOptions.Stage). Enabled only when the scene itself is ready
// to launch (same cast/anchor requirements). A scene with no stage list (a non-linear graph) shows nothing.
function renderAnimations(s, fit) {
  const stages = s.stages || [];
  if (!stages.length) return "";
  const disabled = !state.ready || fit.state !== "ready";
  const rows = stages.map((st) => {
    const label = st.name || `Stage ${st.index}`;
    const tags = st.tags.slice(0, 3).map((t) => `<span class="pill">${esc(t)}</span>`).join("");
    return `<div class="anim-row"><div class="anim-main"><span class="anim-name">${esc(label)}</span><div class="anim-tags">${tags}</div></div><button class="anim-play" type="button" data-play-stage="${escAttr(String(st.index))}" ${disabled ? "disabled" : ""} title="Play this animation">▶</button></div>`;
  }).join("");
  return `<div class="brief-section"><div class="section-label">ANIMATIONS <span class="muted">${stages.length}</span></div><div class="anim-list">${rows}</div></div>`;
}

function renderRoleSockets(s) {
  const members = castMembers();
  const count = Math.max(s.actorCount || 0, s.roles.length, members.length);
  const out = [];
  for (let i = 0; i < count; i++) {
    const role = s.roles[i] || { name: `extra ${i + 1}`, gender: "any" };
    const member = members[i];
    const over = i >= (s.actorCount || s.roles.length);
    out.push(`<div class="role-socket ${member ? "filled" : ""} ${over ? "extra" : ""}"><span class="role-name">${esc(role.name || `role ${i + 1}`)}</span><span class="role-gender">${esc((role.gender || "any").toUpperCase())}</span><strong>${member ? esc(member.name) : "EMPTY"}</strong></div>`);
  }
  return out.join("");
}

function renderAnchorRequirement(s) {
  if (!s.requiresFurniture) return `<div class="anchor-line ready">Free placement. No furniture anchor required.</div>`;
  if (state.furniture) return `<div class="anchor-line ready">Using ${esc(state.furniture.name)}${distanceText(state.furniture)}</div>`;
  return `<div class="anchor-line need">Furniture or marker anchor required.</div>`;
}

function renderAuthorBlock(s) {
  return `<div class="brief-section author-block"><div class="section-label">AUTHOR DATA</div>${kv("SOURCE", s.sourceFile || "live registry")}${kv("PRIORITY", String(s.priority))}${kv("WEIGHT", String(s.weight))}${kv("MATCHMAKING", s.unlisted ? "unlisted" : "listed")}${kv("STRIP", s.policy.stripActors)}${kv("LOCK", s.policy.lockPlayer)}${kv("FADE", s.policy.fade)}${kv("CAMERA", s.policy.camera)}</div>`;
}

function renderLiveDirector() {
  const el = $("liveDirector");
  if (!state.lastHandle) { el.classList.add("hidden"); el.innerHTML = ""; return; }
  const s = sceneById(state.lastSceneId) || sceneById(state.selectedId);
  el.classList.remove("hidden");
  el.innerHTML = `<div class="live-head"><span class="live-dot"></span><div><span>LIVE TAKE</span><strong>${esc(s ? s.title : state.lastSceneId)}</strong></div><b>#${state.lastHandle}</b></div><div class="live-grid"><span>Cast ${castTokens().length}</span><span>${s ? esc(shapeLabel(s)) : "Runtime"}</span><span>${state.furniture ? "Anchored" : "Free"}</span></div>`;
}

function updateLaunch() {
  const s = sceneById(state.selectedId);
  let reason = "";
  if (!state.ready) reason = "Engine not connected.";
  else if (!s) reason = "Select a scene.";
  else {
    const fit = sceneFit(s);
    if (fit.state !== "ready") reason = fit.reason;
  }
  $("play").disabled = reason !== "";
  $("stop").disabled = !state.lastHandle;
  $("launchReason").textContent = reason;
}

function doLaunch(stageIndex) {
  const s = sceneById(state.selectedId);
  if (!s) return;
  const opts = { strip: Number($("optStrip").value), lockPlayer: Number($("optLock").value), camera: $("optCamera").value, speed: Number($("optSpeed").value) };
  // Optional: enter directly on one browsable animation (a stage of the sequence). 0 = the whole scene.
  const stage = Number.isInteger(stageIndex) ? stageIndex : 0;
  if (stage > 0) opts.stage = stage;
  const payload = { sceneId: s.id, castTokens: castTokens(), opts };
  const roleNames = launchRoleNames(s);
  if (roleNames.length === castTokens().length) payload.roleNames = roleNames;
  if (s.requiresFurniture && state.furniture) payload.furnitureToken = state.furniture.token;
  const what = stage > 0 ? `${s.title} · ${stageLabel(s, stage)}` : s.title;
  notice("info", `Launching "${what}"...`);
  send("osf.launch", payload);
}

// Display label for a stage index within a scene (its `name`, else a positional fallback).
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
  notice("info", `Stopping handle ${state.lastHandle}...`);
  state.lastHandle = 0;
  renderAll();
}

function buildTagCloud() {
  const counts = new Map();
  for (const s of state.catalog) {
    if (!unlistedVisible(s)) continue;
    for (const t of s.tags || []) {
      const k = t.toLowerCase();
      counts.set(k, (counts.get(k) || 0) + 1);
    }
  }
  const tags = [...counts.keys()].sort();
  $("tagCount").textContent = String(tags.length);
  $("tagCloud").innerHTML = tags.map((t) => `<button class="tag ${state.filters.tags.has(t) ? "is-on" : ""}" data-tag="${escAttr(t)}" type="button">${esc(t)}</button>`).join("");
  $("tagCloud").querySelectorAll("[data-tag]").forEach((btn) => {
    btn.onclick = () => {
      const tag = btn.dataset.tag;
      if (state.filters.tags.has(tag)) state.filters.tags.delete(tag);
      else state.filters.tags.add(tag);
      buildTagCloud();
      renderAll();
    };
  });
}

function wireFilters() {
  $("search").addEventListener("input", (e) => { state.filters.search = e.target.value.trim().toLowerCase(); renderAll(); });
  segGroup("viewFilter", "view", (v) => { state.filters.view = v; renderAll(); });
  segGroup("countFilter", "count", (v) => { state.filters.count = v; renderAll(); });
  segGroup("anchorFilter", "anchor", (v) => { state.filters.anchor = v; renderAll(); });
  $("showUnlisted").addEventListener("change", (e) => {
    state.filters.showUnlisted = e.target.checked;
    buildTagCloud();
    const selected = sceneById(state.selectedId);
    if (selected && !unlistedVisible(selected)) state.selectedId = bestInitialScene();
    renderAll();
  });
  $("authorMode").addEventListener("change", (e) => { state.filters.authorMode = e.target.checked; renderAll(); });
}

function syncUnlistedFilter() {
  const anyUnlisted = state.catalog.some((s) => s.unlisted);
  const anyListed = state.catalog.some((s) => !s.unlisted);
  if (anyUnlisted && !anyListed) state.filters.showUnlisted = true;
  $("unlistedField").classList.toggle("hidden", !anyUnlisted);
  $("showUnlisted").checked = state.filters.showUnlisted;
}

function segGroup(id, attr, cb) {
  const group = $(id);
  group.querySelectorAll(".seg-btn").forEach((btn) => {
    btn.onclick = () => {
      group.querySelectorAll(".seg-btn").forEach((b) => b.classList.remove("is-on"));
      btn.classList.add("is-on");
      cb(btn.dataset[attr]);
    };
  });
}

function roleLine(s) {
  if (!s.roles || !s.roles.length) return `${s.actorCount || "?"} positional roles`;
  return s.roles.map((r) => `${r.name || "role"}${r.gender && r.gender !== "any" ? ":" + r.gender[0] : ""}`).join(" -> ");
}

function shapeLabel(s) {
  const shape = s.shape || {};
  const kind = String(shape.kind || "linear").toUpperCase();
  const n = shape.kind === "graph" ? (shape.nodes || 0) : (shape.stages || 0);
  const unit = shape.kind === "graph" ? "NODE" : "STAGE";
  const branch = shape.branches ? ` / ${shape.branches} CHOICE${shape.branches === 1 ? "" : "S"}` : "";
  return `${kind} ${n || 1} ${unit}${n === 1 ? "" : "S"}${branch}`;
}

function timelinePips(s) {
  const shape = s.shape || {};
  const count = Math.max(1, Math.min(8, shape.kind === "graph" ? (shape.nodes || 1) : (shape.stages || 1)));
  const pips = Array.from({ length: count }, (_, i) => `<span class="${i === 0 ? "entry" : ""}"></span>`).join("");
  return `<div class="timeline-pips">${pips}${shape.branches ? `<b>${shape.branches}</b>` : ""}</div>`;
}

function kv(label, value) { return `<div class="kv"><span>${esc(label)}</span><strong>${esc(value)}</strong></div>`; }
function distanceLabel(item) { return typeof item.distance === "number" ? `${Math.max(1, Math.round(item.distance))}m` : ""; }
function distanceText(item) { const label = distanceLabel(item); return label ? `<small>${label}</small>` : ""; }
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

function init() {
  wireFilters();
  $("refresh").onclick = () => { notice("info", "Refreshing catalog..."); requestCatalog(true); };
  $("addActor").onclick = () => send("osf.pickCrosshair", { slot: "actor" });
  $("pickFurniture").onclick = () => send("osf.pickCrosshair", { slot: "furniture" });
  $("clearFurniture").onclick = () => { state.furniture = null; renderAll(); };
  $("scanActors").onclick = () => scanNearby("actor");
  $("scanActorsMini").onclick = () => scanNearby("actor");
  $("scanFurniture").onclick = () => scanNearby("furniture");
  $("scanFurnitureMini").onclick = () => scanNearby("furniture");
  $("play").onclick = () => doLaunch();
  $("stop").onclick = doStop;
  $("optSpeed").addEventListener("input", (e) => { $("speedVal").textContent = `${Number(e.target.value).toFixed(1)}x`; });
  renderAll();
  if (bridgeAvailable()) {
    setLamp("wait");
    $("statusText").textContent = "WAITING FOR RUNTIME...";
    requestCatalog(true);
  } else {
    setLamp("ok");
    $("statusText").textContent = "STANDALONE MOCK";
    state.ready = true;
    state.nearbyActors = MOCK_ACTORS.slice();
    state.nearbyFurniture = MOCK_ANCHORS.slice();
    handleCatalog(MOCK_CATALOG);
    notice("info", "Standalone mode. Mock catalog, native calls are stubbed.");
  }
}

document.addEventListener("mousemove", (e) => {
  const c = $("cursor");
  c.style.left = `${e.clientX}px`;
  c.style.top = `${e.clientY}px`;
});

const MOCK_CATALOG = [
  { id: "solo.calibration", title: "Solo Calibration", tags: ["test", "solo", "free"], actorCount: 1, genders: ["any"], requiresFurniture: false, shape: { kind: "linear", stages: 1, nodes: 1, branches: 0 }, policy: { stripActors: false, lockPlayer: false, fade: false, camera: "none" }, sourceFile: "Data/OSF/Scenes/test.osf.json" },
  { id: "ge.chair.love", title: "GE Chair Love", tags: ["ge", "chair", "mf", "paired"], actorCount: 2, roles: [{ name: "bottom", gender: "female" }, { name: "top", gender: "male" }], requiresFurniture: true, stageCount: 4, stages: [{ index: 0, name: "Missionary06", tags: ["missionary", "paired"], clipCount: 2 }, { index: 1, name: "Cowgirl07", tags: ["cowgirl", "paired"], clipCount: 2 }, { index: 2, name: "Doggy04", tags: ["doggy", "paired"], clipCount: 2 }, { index: 3, name: "Scissors02", tags: ["scissors", "paired"], clipCount: 2 }], priority: 20, weight: 4, sourceFile: "Data/OSF/GE/chair.osf.json" },
  { id: "ge.akbunk.sequence", title: "GE AkBunkBed (sequence)", tags: ["ge", "akbunkbed", "mf", "paired", "sequence"], actorCount: 2, roles: [{ name: "left", gender: "female" }, { name: "right", gender: "male" }], requiresFurniture: true, stageCount: 5, stages: [{ index: 0, name: "Blowjob09", tags: ["blowjob", "paired"], clipCount: 2 }, { index: 1, name: "Cowgirl06", tags: ["cowgirl", "paired"], clipCount: 2 }, { index: 2, name: "Doggy17", tags: ["doggy", "paired"], clipCount: 2 }, { index: 3, name: "Missionary18", tags: ["missionary", "paired"], clipCount: 2 }, { index: 4, name: "ReverseCowgirl23", tags: ["reversecowgirl", "paired"], clipCount: 2 }], priority: 30, weight: 2, sourceFile: "Data/OSF/GE/akbunk.osf.json" },
  { id: "pair.freeform", title: "Pair Freeform", tags: ["paired", "free", "demo"], actorCount: 2, genders: ["any", "any"], requiresFurniture: false, shape: { kind: "linear", stages: 3, nodes: 3, branches: 0 }, sourceFile: "Data/OSF/Scenes/demo.osf.json" },
  { id: "author.quest.finale", title: "Quest Finale Branch Test", tags: ["finale", "story", "branching"], actorCount: 2, roles: [{ name: "lead", gender: "any" }, { name: "partner", gender: "any" }], requiresFurniture: false, unlisted: true, shape: { kind: "graph", stages: 3, nodes: 4, branches: 3 }, policy: { stripActors: false, lockPlayer: true, fade: true, camera: "thirdperson_hold" }, sourceFile: "Data/OSF/Author/finale.osf.json" },
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
