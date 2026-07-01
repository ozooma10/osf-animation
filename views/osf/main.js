// ===========================================================================
// OSF Animation — scene director. Talks to OSF Animation's own DLL over OSF
// UI's native bridge (window.osfui), speaking the osf.* contract:
//   web -> native : osf.catalog.get / osf.pickCrosshair / osf.launch / osf.stop
//   native -> web : osf.catalog.data / osf.pick / osf.launchResult
//   platform      : runtime.ready { bridgeVersion } — gate on "0.1"
// Only JSON text crosses the boundary. Runs standalone in a browser (mock data)
// for offline layout work.
// ===========================================================================
"use strict";

const BRIDGE_PROTOCOL = "0.1";
const PLAYER_TOKEN = -1;

// ── State ─────────────────────────────────────────────────────────────────
const state = {
  ready: false,
  catalog: [],          // [{id,title,tags,actorCount,genders,requiresFurniture}]
  selectedId: null,
  partners: [],         // [{token,name}] picked via crosshair (player is implicit)
  furniture: null,      // {token,name} | null
  lastHandle: 0,
  filters: { search: "", count: "any", furn: "any", fitsCast: false, tags: new Set() },
  pendingPick: null,    // "actor" | "furniture" — which slot a pending osf.pick fills
  catalogReceived: false,
};

const $ = (id) => document.getElementById(id);

// ── Bridge plumbing ───────────────────────────────────────────────────────
function bridgeAvailable() {
  return typeof window.osfui === "object" && typeof window.osfui.postMessage === "function";
}

function send(command, fields = {}) {
  const msg = JSON.stringify({ type: "ui.command", payload: { command, ...fields } });
  if (bridgeAvailable()) {
    window.osfui.postMessage(msg);
  } else {
    console.log("(standalone) ->", msg);
    mockNative(command, fields);
  }
}

// The scene browser loads at the MAIN MENU, before OSF Animation registers its
// osf.* commands (that happens on save load / kPostDataLoad). So the first
// osf.catalog.get is often rejected. Keep asking until data arrives (the native
// side also PUSHES the catalog when its bridge becomes ready — whichever wins).
let catalogTimer = null;
let catalogTries = 0;
const CATALOG_MAX_TRIES = 20;   // ~24s of grace for a save to finish loading
function requestCatalog(fresh) {
  if (fresh) { state.catalogReceived = false; catalogTries = 0; }
  clearTimeout(catalogTimer);
  if (state.catalogReceived) return;
  send("osf.catalog.get");
  if (catalogTries < CATALOG_MAX_TRIES) {
    catalogTries++;
    catalogTimer = setTimeout(() => requestCatalog(false), 1200);
  } else {
    notice("err", "No response from OSF Animation. Is it installed, and are you in a loaded game (not the main menu)?");
  }
}

function onNativeMessage(jsonText) {
  let msg;
  try { msg = JSON.parse(jsonText); } catch { return; }
  const { type, payload } = msg;
  switch (type) {
    case "runtime.ready":   handleReady(payload); break;
    case "osf.catalog.data": handleCatalog(payload); break;
    case "osf.pick":         handlePick(payload); break;
    case "osf.launchResult": handleLaunchResult(payload); break;
    default: break; // unknown native messages: ignore, never eval
  }
}

// Publish the inbound handler before anything sends (queued messages flush here).
window.osfui = window.osfui || {};
window.osfui.onMessage = onNativeMessage;

// ── Native message handlers ───────────────────────────────────────────────
function handleReady(p) {
  const bv = p && p.bridgeVersion;
  if (!bv || !bv.startsWith("0.")) {
    setLamp("off");
    $("statusText").textContent = `UNSUPPORTED BRIDGE ${bv || "?"}`;
    notice("err", `This view needs bridge protocol ${BRIDGE_PROTOCOL}; runtime reports ${bv}. Update OSF UI.`);
    return;
  }
  state.ready = true;
  setLamp("ok");
  $("statusText").textContent = `${(p.plugin || "OSF UI").toUpperCase()} v${p.version || "?"}`;
  notice("ok", `Bridge online · protocol ${bv}`);
  requestCatalog(true);
}

function handleCatalog(list) {
  state.catalogReceived = true;
  clearTimeout(catalogTimer);
  state.catalog = Array.isArray(list) ? list : [];
  $("sceneCount").textContent = `${state.catalog.length} SCENES`;
  buildTagCloud();
  renderGrid();
  if (state.selectedId && !state.catalog.some((s) => s.id === state.selectedId)) {
    state.selectedId = null;
  }
  renderInspector();
}

function handlePick(p) {
  if (!p || !p.valid || !p.token) {
    notice("err", `Nothing valid under the crosshair for the ${state.pendingPick || "target"} slot.`);
    state.pendingPick = null;
    return;
  }
  if (p.slot === "furniture") {
    state.furniture = { token: p.token, name: p.name || "furniture" };
    notice("info", `Furniture: ${state.furniture.name}`);
  } else {
    // Avoid double-adding the exact same picked token.
    if (!state.partners.some((c) => c.token === p.token)) {
      state.partners.push({ token: p.token, name: p.name || "actor" });
    }
    notice("info", `Cast + ${p.name || "actor"}`);
  }
  state.pendingPick = null;
  renderInspector();
}

function handleLaunchResult(p) {
  if (p && p.ok && p.handle) {
    state.lastHandle = p.handle;
    notice("ok", `Playing "${sceneTitle(p.sceneId)}" · handle ${p.handle}`);
  } else {
    notice("err", (p && p.error) || "Launch failed.");
  }
  renderInspector();
}

// ── Rendering ─────────────────────────────────────────────────────────────
function sceneById(id) { return state.catalog.find((s) => s.id === id) || null; }
function sceneTitle(id) { const s = sceneById(id); return s ? s.title : id; }

function matchesFilters(s) {
  const f = state.filters;
  if (f.search) {
    const hay = `${s.title} ${s.id} ${(s.tags || []).join(" ")}`.toLowerCase();
    if (!hay.includes(f.search)) return false;
  }
  if (f.count !== "any") {
    const n = s.actorCount || 0;
    if (f.count === "3" ? n < 3 : n !== Number(f.count)) return false;
  }
  if (f.furn === "yes" && !s.requiresFurniture) return false;
  if (f.furn === "no" && s.requiresFurniture) return false;
  if (f.fitsCast && (s.actorCount || 0) !== castTokens().length) return false;
  if (f.tags.size) {
    const st = new Set((s.tags || []).map((t) => t.toLowerCase()));
    let hit = false;
    for (const t of f.tags) if (st.has(t)) { hit = true; break; }
    if (!hit) return false;
  }
  return true;
}

function renderGrid() {
  const grid = $("grid");
  const results = state.catalog.filter(matchesFilters);
  $("resultCount").textContent = `${results.length}/${state.catalog.length}`;
  $("empty").classList.toggle("hidden", results.length > 0);
  grid.innerHTML = "";
  for (const s of results) {
    const card = document.createElement("div");
    card.className = "card" + (s.id === state.selectedId ? " is-sel" : "");
    card.setAttribute("role", "listitem");
    card.onclick = () => { state.selectedId = s.id; renderGrid(); renderInspector(); };

    const genders = uniqueGenders(s.genders);
    card.innerHTML =
      `<div class="card-title">${esc(s.title)}</div>` +
      `<div class="card-id">${esc(s.id)}</div>` +
      `<div class="card-tags">${(s.tags || []).slice(0, 5).map((t) => `<span class="pill">${esc(t)}</span>`).join("")}</div>` +
      `<div class="card-badges">` +
        `<span class="badge cast">${s.actorCount || "?"}&nbsp;CAST</span>` +
        (genders ? `<span class="badge gender">${esc(genders)}</span>` : "") +
        (s.requiresFurniture ? `<span class="badge furn">FURNITURE</span>` : "") +
      `</div>`;
    grid.appendChild(card);
  }
}

function renderInspector() {
  const s = sceneById(state.selectedId);
  const detail = $("detail");
  if (!s) {
    detail.innerHTML = `<p class="detail-empty">SELECT A SCENE TO INSPECT.</p>`;
  } else {
    const genders = uniqueGenders(s.genders);
    detail.innerHTML =
      `<h2>${esc(s.title)}</h2>` +
      `<div class="d-id">${esc(s.id)}</div>` +
      `<div class="d-tags">${(s.tags || []).map((t) => `<span class="pill">${esc(t)}</span>`).join("") || "<span class='d-id'>no tags</span>"}</div>` +
      `<div class="d-req">` +
        row("CAST NEEDED", String(s.actorCount || "?")) +
        (genders ? row("GENDERS", genders) : "") +
        row("FURNITURE", s.requiresFurniture ? "REQUIRED" : "NONE") +
      `</div>`;
  }
  renderCast();
  renderFurniture();
  updateLaunch();
}

function renderCast() {
  const list = $("castList");
  list.innerHTML = "";
  list.appendChild(castSlot("PLAYER", "You", null, true));
  state.partners.forEach((c, i) => {
    list.appendChild(castSlot(`ACTOR ${i + 1}`, c.name, () => {
      state.partners.splice(i, 1);
      renderInspector();
    }, false));
  });
}

function castSlot(role, who, onDrop, isPlayer) {
  const li = document.createElement("li");
  li.className = "cast-slot" + (isPlayer ? " player" : "");
  li.innerHTML = `<span class="role">${role}</span><span class="who">${esc(who)}</span>`;
  if (onDrop) {
    const b = document.createElement("button");
    b.className = "drop"; b.type = "button"; b.textContent = "×";
    b.title = "remove"; b.onclick = onDrop;
    li.appendChild(b);
  }
  return li;
}

function renderFurniture() {
  const slot = $("furnSlot");
  if (state.furniture) {
    slot.innerHTML = `<span class="who">${esc(state.furniture.name)}</span>`;
    $("clearFurniture").classList.remove("hidden");
  } else {
    slot.innerHTML = `<span class="slot-empty">NONE</span>`;
    $("clearFurniture").classList.add("hidden");
  }
}

// castTokens: player first, then picked partners, in slot order.
function castTokens() {
  return [PLAYER_TOKEN, ...state.partners.map((c) => c.token)];
}

function updateLaunch() {
  const s = sceneById(state.selectedId);
  const play = $("play");
  const reasonEl = $("launchReason");
  let reason = "";
  if (!state.ready) reason = "Engine not connected.";
  else if (!s) reason = "Select a scene.";
  else if (s.actorCount && castTokens().length !== s.actorCount)
    reason = `Needs ${s.actorCount} cast; you have ${castTokens().length}. Add crosshair actors.`;
  else if (s.requiresFurniture && !state.furniture)
    reason = "Pick a furniture target.";
  play.disabled = reason !== "";
  reasonEl.textContent = reason;
  $("stop").disabled = !state.lastHandle;
}

// ── Actions ───────────────────────────────────────────────────────────────
function doLaunch() {
  const s = sceneById(state.selectedId);
  if (!s) return;
  const opts = {
    strip: Number($("optStrip").value),
    lockPlayer: Number($("optLock").value),
    camera: $("optCamera").value,
    speed: Number($("optSpeed").value),
  };
  const payload = { sceneId: s.id, castTokens: castTokens(), opts };
  if (state.furniture) payload.furnitureToken = state.furniture.token;
  notice("info", `Launching "${s.title}"…`);
  send("osf.launch", payload);
}

function doStop() {
  if (!state.lastHandle) return;
  send("osf.stop", { handle: state.lastHandle });
  notice("info", `Stopping handle ${state.lastHandle}…`);
  state.lastHandle = 0;
  updateLaunch();
}

// ── Filter UI ─────────────────────────────────────────────────────────────
function buildTagCloud() {
  const counts = new Map();
  for (const s of state.catalog) for (const t of s.tags || []) {
    const k = t.toLowerCase();
    counts.set(k, (counts.get(k) || 0) + 1);
  }
  const cloud = $("tagCloud");
  cloud.innerHTML = "";
  [...counts.keys()].sort().forEach((t) => {
    const el = document.createElement("span");
    el.className = "tag" + (state.filters.tags.has(t) ? " is-on" : "");
    el.textContent = t;
    el.onclick = () => {
      if (state.filters.tags.has(t)) state.filters.tags.delete(t);
      else state.filters.tags.add(t);
      el.classList.toggle("is-on");
      renderGrid();
    };
    cloud.appendChild(el);
  });
}

function wireFilters() {
  $("search").addEventListener("input", (e) => {
    state.filters.search = e.target.value.trim().toLowerCase();
    renderGrid();
  });
  segGroup("countFilter", "count", (v) => { state.filters.count = v; renderGrid(); });
  segGroup("furnFilter", "furn", (v) => { state.filters.furn = v; renderGrid(); });
  $("fitsCast").addEventListener("change", (e) => {
    state.filters.fitsCast = e.target.checked; renderGrid();
  });
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

// ── Helpers ───────────────────────────────────────────────────────────────
function row(label, value) {
  return `<div class="d-line"><span>${label}</span><span>${esc(value)}</span></div>`;
}
function uniqueGenders(genders) {
  if (!Array.isArray(genders) || !genders.length) return "";
  const set = [...new Set(genders.filter((g) => g && g !== "any"))];
  if (!set.length) return "";
  return set.join(" · ").toUpperCase();
}
function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}
function setLamp(stateName) { $("lamp").dataset.state = stateName; }
let noticeTimer = null;
function notice(kind, text) {
  const el = $("notice");
  el.className = `notice ${kind}`;
  el.textContent = text;
  clearTimeout(noticeTimer);
  if (kind !== "err") noticeTimer = setTimeout(() => { el.className = "notice"; el.textContent = ""; }, 6000);
}

// ── Wire the DOM ──────────────────────────────────────────────────────────
function init() {
  wireFilters();
  $("refresh").onclick = () => { notice("info", "Refreshing catalog…"); requestCatalog(true); };
  $("addActor").onclick = () => { state.pendingPick = "actor"; send("osf.pickCrosshair", { slot: "actor" }); };
  $("pickFurniture").onclick = () => { state.pendingPick = "furniture"; send("osf.pickCrosshair", { slot: "furniture" }); };
  $("clearFurniture").onclick = () => { state.furniture = null; renderInspector(); };
  $("play").onclick = doLaunch;
  $("stop").onclick = doStop;
  $("optSpeed").addEventListener("input", (e) => { $("speedVal").textContent = `${Number(e.target.value).toFixed(1)}×`; });

  renderInspector();

  if (bridgeAvailable()) {
    setLamp("wait");
    $("statusText").textContent = "WAITING FOR RUNTIME…";
    // Nudge in case runtime.ready already fired before this script ran; retries
    // cover the common case where OSF Animation registers only after save load.
    requestCatalog(true);
  } else {
    // Standalone: fabricate a catalog so layout is testable in a browser.
    setLamp("ok");
    $("statusText").textContent = "STANDALONE (NO BRIDGE)";
    state.ready = true;
    handleCatalog(MOCK_CATALOG);
    notice("info", "Standalone mode — mock catalog, native calls are stubbed.");
  }
}

// Software pointer + key echo (OS cursor hidden in-game).
document.addEventListener("mousemove", (e) => {
  const c = $("cursor");
  c.style.left = `${e.clientX}px`;
  c.style.top = `${e.clientY}px`;
});

// ── Standalone mock (never used when the bridge is live) ────────────────────
const MOCK_CATALOG = [
  { id: "solo", title: "solo (base-framework playback check)", tags: ["test"], actorCount: 1, genders: [], requiresFurniture: false },
  { id: "pair", title: "pair", tags: ["test"], actorCount: 2, genders: ["any", "any"], requiresFurniture: false },
  { id: "ge.abb", title: "GE AkBunkBed (sequence)", tags: ["ge", "akbunkbed", "mf", "paired", "sequence"], actorCount: 2, genders: ["male", "female"], requiresFurniture: true },
  { id: "ge.chl", title: "GE Chair Love", tags: ["ge", "chair", "mf", "paired"], actorCount: 2, genders: ["male", "female"], requiresFurniture: true },
];
function mockNative(command, fields) {
  if (command === "osf.catalog.get") setTimeout(() => handleCatalog(MOCK_CATALOG), 60);
  else if (command === "osf.pickCrosshair")
    setTimeout(() => handlePick({ slot: fields.slot, valid: true, token: Math.floor(Math.random() * 900 + 100), name: fields.slot === "furniture" ? "AkBunkBed" : "Sarah Morgan", formId: 0x12a57a }), 60);
  else if (command === "osf.launch")
    setTimeout(() => handleLaunchResult({ ok: true, handle: 42, sceneId: fields.sceneId }), 80);
}

init();
