# Emote Wheel — Plan

## Goal

Add a **wheel mode** to the existing `views/osf` browser view: opened by the
`openWheel` hotkey verb, it shows a radial menu of emote scenes (catalog entries
filtered by a tag prefix, default `player.emote.`), plays the picked scene on the
player — or on the crosshair-targeted NPC if one was captured at open time — then
closes. This replaces the stub `API::OpenWheel` left by Hotkeys_Plan.md.

Content (the actual `player.emote.*` scenes) is SitLean_Plan.md's job — this plan is
pure mechanism and must work against ANY scenes carrying the tag prefix (test with a
throwaway pack).

## Constraints

- Read `AGENTS.md` first. Build chain is Windows-only; this dev box is a Mac —
  **you cannot compile or run the view in-game.** The view has a standalone mock path
  (`views/osf/main.js:1499-1508`, mock data L1514-1577) — extend the mock so the wheel
  can be exercised in a plain browser; that is your primary verification.
- The osf view is hosted by the external **OSFUI.dll** (this repo is only a consumer:
  `src/API/OSFUI_API.h`). Show/hide happens ONLY via `g_ui->RequestMenu("osf", open)`
  (bridge MINOR ≥ 1). **The view cannot close itself today** — closing is host-driven
  (`main.js:142-145` only *reports* visibility) — so this plan adds a native
  close-request command.
- `manifest.json` declares `pausesGame:false` — the wheel is a live overlay; keep it
  lightweight (no world-obscuring backdrop).

## Verified facts

- Web→native: all commands go out under a `ui.command` envelope via `send()`
  (`main.js:59-63`); native side registers handlers with
  `g_ui->RegisterCommand("osf.<name>", ...)` (`UIBridge.cpp:1027-1037`).
- Native→web: `SendJson(view, type, payload)` (`UIBridge.cpp:141-148`) →
  `g_ui->SendToWeb`. Inbound dispatch is the `switch` in `onNativeMessage`
  (`main.js:126-148`).
- Crosshair capture pattern: `OnPickCrosshair` (`UIBridge.cpp:454-493`) reads
  `player->commandTarget` (L466), validates `Is(kREFR)||Is(kACHR)` + `IsActor()`,
  allocates a token via `AllocToken` (L62-73; formID-keyed, reused across scans;
  **-1 = player, never stored**). `ResolveToken` (L185-200) re-validates via
  `TESForm::LookupByID` + `IsDeleted()`. **No dead/hostile checks exist in pick** —
  the scan path checks `IsDead()` (L726); the wheel target capture must add gates.
- Launch path: `OnLaunch` (`UIBridge.cpp:495-615`) takes
  `{sceneId, castTokens[], opts{...}, roleNames?, furnitureToken?}`, resolves tokens,
  replace-in-place stops a busy actor's scene (L574-583), starts via `IOSFSceneAPI`,
  replies `osf.launchResult {ok, handle | error}`. **An emote is just this with
  `castTokens:[-1]` (player) or `[targetToken]` (NPC) — no new launch native.**
- Catalog: `state.catalog` (authored lane) + `state.library` (vanilla lane) are
  already client-side (`main.js:12,40`); entries carry `id`, `title`, `tags[]`,
  `actorCount`, `genders[]`, `species`, `estSec` (`UIBridge.cpp:421-436`). The wheel
  filters entries whose `tags[]` contain a tag starting with the prefix — solo
  (`actorCount === 1`) only.
- Mode state: `state.mode` is `"scenes" | "library"` (`main.js:39`, `setMode`
  L501-510). The wheel becomes a third, transient mode.
- Visibility handshake: host pushes `ui.visibility`; the view relays
  `osf.opened`/`osf.closed` (`main.js:142-145`), which natively drive
  `InputService::SetUiCursorVisible` (`UIBridge.cpp:930-939`).
- Keyboard: the view receives arrow/Enter keys (OSF UI injects gamepad as those,
  `main.js:1281-1289`); `onNavKey` (L1368-1396) handles them. Escape handling is
  net-new — **Phase-0 spike result on key delivery applies; verify Escape reaches
  the view in-game before relying on it, and provide a click/back affordance too.**

## Design

### 1. Native: `API::OpenWheel(const std::string& a_tagPrefix)` (UIBridge)

Replace the Hotkeys_Plan stub. On the game thread (the hotkey verb already posts
there):

1. **Capture target** from `player->commandTarget` using the `OnPickCrosshair`
   validation (extract the shared part into a file-local helper rather than
   duplicating): must be `kACHR`/`IsActor`. Additional wheel gates: reject
   `IsDead()`; reject when the actor is in combat (grep for the combat-check idiom
   used elsewhere in the repo and reuse; if none exists, `IsInCombat()`); reject
   non-`human` species unless the emote pool proves otherwise
   (`Util::ActorSpecies`, same as pick L489). Rejection ⇒ wheel opens in
   player-only mode (target fields absent), NOT an error.
2. Stash pending wheel state (file-static like `g_lastHandle`):
   `{active, tagPrefix, targetToken, targetName, targetFormId}`.
3. Open the view exactly like `OpenBrowser()` (`UIBridge.cpp:975-1008`): same
   version gate (MINOR ≥ 1 for `RequestMenu`), same hide of
   Book/Inventory/Data menus, then `RequestMenu("osf", true)`.
4. **Mode delivery must survive the open race**: send
   `SendJson(kViewId, "osf.mode", {mode:"wheel", tagPrefix, target:{token,name} | null})`
   immediately AND re-send it in the `osf.opened` handler while pending-wheel is
   active (idempotent). Clear pending state in `osf.closed`.
5. `OpenBrowser()` must also clear any stale pending-wheel state so a normal F10
   open never lands in wheel mode.

### 2. Native: close request

New command `osf.requestClose` (register next to the others at
`UIBridge.cpp:1027-1037`): handler calls `RequestMenu(kViewId, false)` on the game
thread (mirror the OpenBrowser task pattern). Generic — the browser could use it
later; the wheel uses it for cancel and after-pick.

### 3. View: wheel mode (`views/osf/main.js`, `index.html`, `style.css`)

- `onNativeMessage` gains `case "osf.mode"` → `enterWheel(payload)` /
  or `exitWheel()` when `payload.mode !== "wheel"`. `ui.visibility hide` and
  `osf.closed` relay also `exitWheel()` (restore `state.mode` to `"scenes"` and
  re-render), so a reopened browser is never stuck in wheel mode.
- New root container `#wheel` in `index.html` (sibling of `.console`), hidden by
  default; entering wheel mode hides `.console`/`#brief`/`#rail` and shows `#wheel`.
  Keep the world visible: translucent center, no full-screen backdrop.
- **Wheel content**: filter `state.catalog` (authored lane only) for solo scenes with
  a tag starting at `tagPrefix`; label = `title`; cap at 12 slices; more → a second
  ring or page-dots (pick the simplest; log-drop beyond the cap is acceptable v1 —
  note it in the plan's "no silent caps" spirit with a small "+N more" indicator).
  Empty pool → show "no emotes installed" + close affordance.
- **Layout**: CSS-positioned slices around a circle (`transform: rotate/translate`)
  — no canvas needed; style.css has no radial precedent, so net-new CSS scoped under
  `#wheel`. Show the target's name in the hub when a target was captured
  ("→ Sarah" vs "You").
- **Input**: reuse the existing nav idiom — arrow keys move slice focus
  (`onNavKey` already routes arrows; add wheel-mode branch), Enter/click picks,
  Escape/right-click/hub-click sends `osf.requestClose`. Mouse hover focuses slices.
- **Pick**: send the existing `osf.launch` with
  `{sceneId, castTokens:[target ? target.token : -1]}` (no opts overrides — emote
  scenes carry their own camera/strip defaults). On `osf.launchResult ok` →
  `send("osf.requestClose")`; on error → show the error in the hub, stay open.
- Extend the standalone mock (`main.js:1514-1577`): a mocked `osf.mode` wheel entry
  with fake emote entries and a fake target, so the wheel renders in a plain browser.

### 4. Hotkey glue

`HotkeyService`'s `kOpenWheel` verb (stubbed in Hotkeys_Plan) now calls
`API::OpenWheel(argOrDefault("player.emote."))`. Keep the graceful degrade: OSF UI
absent/too old → HUD error (reuse OpenBrowser's logging).

## Out of scope

- Emote/sit-lean scene content (SitLean_Plan.md).
- Paired player+NPC response scenes (later content addition; the launch shape
  already supports it by sending two launches).
- A second Ultralight view id — wheel is a mode of the existing "osf" view.
- Hold-key-to-open/release-to-pick UX (needs key-up events; v1 is press-to-open,
  click/Enter to pick).

## Acceptance criteria

1. `API::OpenWheel` captures + gates the target, opens the view, and delivers
   `osf.mode` reliably (including the resend-on-`osf.opened` path); `OpenBrowser`
   clears pending wheel state.
2. `osf.requestClose` registered and functional; wheel closes on pick-success,
   Escape, and cancel; browser (F10) behavior unchanged.
3. Wheel renders from the catalog by tag prefix, launches on player (`-1`) or target
   token via the EXISTING `osf.launch`, and shows launch errors without closing.
4. Wheel mode never leaks: after any close, reopening the browser shows the normal
   console UI.
5. Mock path exercises the wheel in a plain browser (document how in the view README).
6. AGENTS.md / docs updated where they enumerate `osf.*` commands and message types;
   CHANGELOG entry.

## Verification

- Mac (you): open `views/osf/index.html` standalone → mock wheel renders, keyboard
  nav + pick + cancel paths run, no console errors.
- Windows + in-game (user): hotkey opens wheel with/without a crosshair NPC; pick
  plays emote on self and on target; dead/combat NPC → player-only mode; Escape
  closes; F10 browser unaffected; `cgf "OSF.Health"` passes.
