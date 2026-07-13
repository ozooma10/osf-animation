# Emote-Wheel Pinning + Adaptive Wheel Geometry

## Context

Two user complaints about the emote wheel, plus a label fix:

1. **No user control over wheel content.** The wheel pool is hard-wired: solo scenes tagged `player.emote.*` ([main.js:1211](views/osf/main.js#L1211)), priority-sorted, capped at 12 with overflow silently dropped. Users want to choose which animations ride the wheel. **Decided with user:** pin toggle in the F10 browser, any solo authored scene pinnable (not just emote-tagged; vanilla library lane out of scope v1 — it's fetch-on-demand and may not be loaded when the wheel opens), DLL persists the list (view has `filesystem:false`).
2. **Layout feels stretched/lopsided.** `WHEEL_RX=250 / WHEEL_RY=170` is tuned for 12 crowded slices, but the real emotes pack ships ~5 — a wide flat oval instead of a wheel. Fix: rounder, count-adaptive geometry.
3. ~~Card label~~ — **already done**: `views/osf/manifest.json` title → "OSF Animation Browser".

**Pool semantics:** any pins exist → wheel shows exactly the pins, in pin order, ignoring tagPrefix (explicit user intent). No pins → today's tag-prefix pool (works out of the box).

## Step 1 — New native module `src/Serialization/WheelPins.{h,cpp}`

Persistent ordered list of pinned scene ids at `<Documents>\My Games\Starfield\OSF\wheel-pins.json`, shape `{ "version": 1, "pins": ["scene/id", ...] }`. **Copy the idiom from [FirstRunHint.cpp](src/UI/FirstRunHint.cpp)** (the canonical small-store): anon-namespace `std::mutex g_lock` + `bool g_loaded` + `std::vector<std::string> g_pins`; `StatePath()` via `SFSE::log::log_directory()->parent_path().parent_path() / "OSF" / "wheel-pins.json"` in try/catch; `LoadLocked()` tolerant parse (`allow_exceptions=false, ignore_comments=true`), keep unique non-empty strings; `SaveLocked()` `.tmp` + atomic `std::filesystem::rename`. xmake globs `src/**.cpp` — no build-file change.

Public API:
```cpp
namespace OSF::Serialization::WheelPins {
    int  Order(std::string_view a_sceneId);              // 1-based pin position, 0 = unpinned
    bool Set(std::string_view a_sceneId, bool a_pinned); // append / remove (later pins shift up); saves on change
}
```
Stale ids (uninstalled pack) are **kept in the file, never pruned** — they revive on reinstall; filtering happens naturally because BuildCatalog only emits `pinned` for scenes that exist. `WheelPins` is a leaf lock (nests inside the registry read lock, same as `ClipDurations::Lookup` at [UIBridge.cpp:385](src/API/UIBridge.cpp#L385)); it must never call the registry.

## Step 2 — `src/API/UIBridge.cpp`

- Include `Serialization/WheelPins.h`.
- `Card` struct (~L317-324): add `int pinned = 0;` next to `unlisted`. Populate in the `ForEachDef` copy (~L359): `c.pinned = a_library ? 0 : Serialization::WheelPins::Order(d.id);` Emit at ~L467 next to `{"unlisted", ...}`: `{ "pinned", c.pinned },`.
- New handler `OnWheelPin` modeled on `OnStop` (L650): noexcept, `ParsePayload`, require string `sceneId`, `bool pinned = j.value("pinned", false)`; on `WheelPins::Set(...)` returning true → `REX::DEBUG("[UI] osf.wheel.pin ...")` + `PushCatalogUpdate()` (L1016 — full re-push is the reply; view already tolerates unsolicited catalog pushes).
- Register `g_ui->RegisterCommand("osf.wheel.pin", &OnWheelPin, nullptr);` in `InstallUIBridge()` (~L1160).
- No `OpenWheel`/`g_wheel` changes — pool logic is view-side.

## Step 3 — `views/osf/main.js`

- **normalizeScene** (~L321): `pinned: Math.max(0, Math.trunc(Number(raw.pinned) || 0)),`.
- **sceneRow** (L907-916): rows are `<button>`s — no nested button. Add a non-interactive marker when `s.pinned > 0`: `<span class="libx-pinmark" title="On the emote wheel">◆</span>` after `libx-title`. Catalog lane only (library rows untouched).
- **renderBrief** (~L1016-1018): pin toggle in the head. Gate: `const pinnable = !s.library && !s.unlisted && (s.actorCount || 0) === 1;` (`s.library` is stamped at L196; `sceneById` L545 searches both lanes). Button: `data-act="pin-toggle" data-id=...`, text `◇ PIN TO WHEEL` / `◆ ON WHEEL` (class `on` when pinned).
- **togglePin(id)** (new, near `doStop`): `send("osf.wheel.pin", {sceneId:id, pinned:on})` + optimistic local update matching the DLL's compaction (pin → max(order)+1; unpin → zero it and decrement higher orders), then `renderAll()`. The authoritative catalog push reconciles moments later (visual no-op).
- **onClick** (~L1357): `case "pin-toggle": togglePin(el.dataset.id); break;`.
- **wheelPool()** (L1211-1218) rewrite: `eligible = catalog.filter(s => !s.unlisted && (s.actorCount||0) === 1)`; `pins = eligible.filter(s => s.pinned > 0).sort((a,b) => a.pinned - b.pinned)`; if `pins.length` return pins (ignores tagPrefix); else legacy tag-prefix filter + priority/weight/title sort. All-stale pins → empty → automatic tag fallback. Ascending sort tolerates order gaps from stale ids (never show rank numbers, only on/off state).
- **renderWheel** (L1220-1254):
  - Stash `w.pool = slices;` and change `wheelPick(i)` (L1270) to read `(w.pool || [])[i]` — fixes a latent render-vs-pick re-pool race (catalog push between render and click could shift indices).
  - Replace `WHEEL_RX/RY` constants (L1183) with count-adaptive geometry:
    ```js
    function wheelGeom(n) {
      const t = Math.max(0, Math.min(1, (n - 3) / 9));  // <=3 slices -> 0, 12 -> 1
      return { rx: Math.round(150 + 100 * t), ry: Math.round(140 + 50 * t) };  // 150x140 .. 250x190
    }
    ```
    Emit as CSS vars on the ring: `<div class="wheel-ring" style="--wrx:${rx}px;--wry:${ry}px">` and use rx/ry for slice transforms. (Rounder than today's 250×170 even at full count; near-circle at 5, still clears the 128px hub.)
  - Overflow copy: `+N pinned not shown` when the pool is pins, else `+N more not shown`.
- **Mock** (L1704-1778): `let MOCK_PINS = ["emote.uninstalled"]` (pre-seeded stale id — must never render and must not block fallback); `mockCatalog()` maps `pinned: MOCK_PINS.indexOf(s.id) + 1`; `osf.catalog.get` serves `mockCatalog()`; new `osf.wheel.pin` branch mutates MOCK_PINS and re-pushes via `setTimeout(() => handleCatalog(mockCatalog()), 60)`.

## Step 4 — `views/osf/style.css` (wheel block L540-568 + new rules)

- `.wheel-ring`: add `--wrx:250px;--wry:170px` defaults (JS overrides).
- `.wheel-dial`: `width:calc(var(--wrx)*2 - 30px); height:calc(var(--wry)*2 - 30px)` (drops hardcoded 470/310; identical at old radii).
- `.wheel-more`: `top:calc(var(--wry) + 36px)` (was 206 = 170+36).
- New `.pin-btn` (chip-btn-adjacent styling: mono 9.5px, `--line-strong` border, `.on` = accent border/color) and `.libx-pinmark` (accent, 9px, flex:none). Update the "radii match WHEEL_RX/RY" comment.

## Step 5 — Docs

- [views/osf/README.md](views/osf/README.md): add `osf.wheel.pin {sceneId,pinned}` to the contract list; rewrite the emote-wheel section's pool sentence (pins-first in pin order, any solo authored scene, overrides tagPrefix; else tag pool); note persistence path (account-global, survives ReloadPacks/reinstall); update standalone-dev section (pin round-trip, pre-seeded stale pin).
- CHANGELOG.md `[Unreleased]`: Added — browser pin toggle + pins-first wheel + `osf.wheel.pin` + `wheel-pins.json`; Changed — wheel ring rounder and sized to emote count; card title now "OSF Animation Browser".
- AGENTS.md: nothing (doesn't enumerate osf.* commands).

## Edge cases

- Uninstalled pack → id stays in file, card never built, view never sees it; all pins stale → tag fallback.
- Pinned scene later multi-actor/unlisted → `eligible` gate drops it from pool; brief hides the button; row glyph still renders when `pinned>0` (honest).
- Catalog push mid-wheel → `renderAll()` → `renderWheel()` re-pools; focus clamp L1227 handles shrink; `w.pool` stash keeps in-flight clicks coherent.
- Corrupt pins file → tolerant parse → empty; next toggle rewrites.

## Verification

**Standalone** (serve `views/osf/` with `python -m http.server` per `.claude/launch.json`):
1. Solo scene brief shows PIN TO WHEEL; multi-actor/library/unlisted show none.
2. Pin 3 in order → rows gain ◆; `W` → wheel shows exactly those 3 in order on a near-circular ring, dial/+N tracking it.
3. Pin 13 → `+1 pinned not shown`; unpin all → 14-emote tag pool returns at 250×190 with `+2 more not shown`.
4. Stale `emote.uninstalled` never renders, never blocks fallback.
5. Facepalm mock-fail, Shift+W player-only, arrows/Enter/Esc all unchanged. No console errors.

**Build:** `xmake` (per AGENTS.md; releasedbg). New .cpp auto-globbed.

**In-game (user):** pin two emotes → `wheel-pins.json` appears with ordered pins; hotkey wheel shows pins only in pin order, rounder ring; unpin all → `player.emote.*` fallback; pins survive ReloadPacks, game restart, and pack disable/re-enable in MO2; SFSE log shows `[UI] osf.wheel.pin` DEBUGs, no WARNs. Launcher card reads "OSF Animation Browser".
