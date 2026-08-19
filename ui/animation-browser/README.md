# OSF Animation — animation browser view

The in-game animation and scene browser/launcher. Its editable source is a TypeScript/Preact app in this directory; `build/osfui-animation-browser/SFSE/Plugins/OSFUI/views/osf.animation/browser/` is generated production output — never committed, rebuilt by every `xmake` and every packaging run. It is rendered by
**OSF UI** and driven **natively** by OSF Animation's own
DLL over OSF UI's bridge API (protocol **2.0**). Only JSON text crosses the
boundary.

## Development and build

Install once with `npm install` in this directory. Use `npm run dev` for the OSF UI browser harness with hot reload, `npm test` for the typed bridge/model tests, `npm run check` for host compatibility and strict TypeScript checks, and `npm run build` to validate and regenerate the production view.

This directory is the only editable copy — `build/osfui-animation-browser/` is disposable toolchain output and is wiped on every build. A native `xmake` re-runs the OSF UI CLI whenever sources here are newer, so a plain `xmake` never deploys a stale view. **Node.js/npm is therefore required to build this project**; there is no committed bundle to fall back on.

## How it's wired

```
ui/animation-browser/src/ ── OSF UI CLI ──► build/osfui-animation-browser/.../views/osf.animation/browser/ ──► OSF UI MessageBridge ──► OSF Animation DLL
   window.osfui.send/request       (2.0 helper)               src/API/UIBridge.cpp
   window.osfui.on/state       ◄── events/state ────────────── osf.animation.* handlers
```

- **Native side:** `src/API/UIBridge.{h,cpp}` (vendored bridge header
  `src/API/OSFUI_API.h`). Registered in `src/main.cpp` at `kPostDataLoad`
  (`OSF::API::InstallUIBridge()`), a no-op when OSF UI is absent.
- **Identity:** mod id `osf.animation`, view name `browser`, qualified view id
  `osf.animation/browser` — the id used by `RegisterView` / `RequestMenu`.
  The manifest declares `kind: "menu"` (focused, input-capturing) and an
  advisory `targetVersion` (the OSF UI version authored against; older hosts
  show a "needs update" badge on the Mods surface, nothing is gated).
- **Contract (`osf.animation.*`):**
  `catalog.get`→`catalog.data`, `library.get`→`library.data`,
  `imports.get`→`imports.data` (the per-file registry-load report behind the
  IMPORTS panel: one record per `*.osf.json` the engine scanned — counts, timing,
  size, and the load problems attributed to it, plus rolled-up `totals`. Fetched
  only while the panel is open, and re-fetched on every open because the report
  describes the *last* load and `ReloadPacks` can have happened since. Problem
  lines are bounded per file; `problemCount` carries the true total).
  `imports.reload`→`imports.reloadResult` runs the same full content refresh
  (registries, gear, and clip caches) as the legacy Papyrus `ReloadPacks`, returning
  the new report so the view can diff new/resolved diagnostics.
  `imports.copy {path}`→`imports.copyResult`
  copies the authoritative, untruncated per-file report through the native Windows
  clipboard path,
  `projectPickables {slot,width,height}`→`pickTargets` (tokened marker
  geometry the view both renders and resolves clicks against), `pickScreen {slot,token}`→`pick`
  (validation only — the token is the hot marker's), `scanNearby`→`scanResults`,
  `anchorMatch`→`anchorMatch` (reply), `launch`→`launchResult`, `stop`.
  A launch may carry additive `singleAnimation:true`; after entering the requested
  `opts.stage`, any scene edge out ends playback instead of continuing through the
  parent registry definition. `inspect:true` bypasses `SceneRuntime` entirely and
  starts a browser-owned, scrub-only Layer-A playback session. It has no lifecycle callbacks,
  cues, sounds, cameras, equipment policy, or external consumers; the browser owns
  temporary render props and reconstructs them from the selected node's
  enter/numeric/end `osf.prop.*` actions at each seek. A preview starts paused;
  `playback.set {handle, paused:false}` runs it at authored speed (looping its clip,
  props reconciled on the playback poll) and `paused:true` freezes it again — still
  side-effect-free, since a preview carries no timers, loop targets, or marks. Any
  seek takes the transport back, so scrubbing or frame-stepping pauses it. An
  authored `hold` stage is previewed as its full clip: a preview is a transport over
  the animation, not a replay of the scene's timing.
  `wheel.get`→`wheel.data`,
  `wheel.set {entries:[{scene,stage?},...]}` (persist the complete ordered animation-wheel loadout)
  or `wheel.set {reset:true}` (return to installed defaults); the reply is an
  unsolicited `catalog.data` re-push carrying fresh wheel state/order fields,
  `orbit {dx,dy,wheel}` (world-drag steers the native orbit camera; no reply),
  `opened`/`closed` (visibility reports off the `ui.visibility` relay), and
  `requestClose` (view asks the host to hide it — used by the animation wheel).
  Native→web `activeScenes {scenes:[{handle, sceneId, stage, inspection, player,
  cast:[{token,name,player}]}]}` is the frozen bridge event carrying the authoritative active-launch
  list: runtime scene instances and preview sessions. It is pushed on `opened`, after a launch, and
  on every relevant lifecycle change (stage advance or any termination, including natural ends).
  The view surfaces it as an **ACTIVE tab** in the browse mode switch (visible only while something
  is active, labeled with the count) holding one card per launch — title, handle, YOU, current stage,
  full cast, per-launch stop (`stop {handle}`), STOP ALL — plus a compact header chip (a single launch
  shows directly with its stop;
  several collapse to a count) that opens the tab, and LIVE badges on busy
  cast. Ordinary runtime timelines are forward-only (pause/resume); a browser
  scene-preview handle adds frame stepping and seeking to the same play/pause. A preview also has
  no runtime stage machine (`advance` no-ops on its handle), so its card carries a
  **stage strip** — ◀ / windowed per-stage chips / ▶, with NEXT ▸ and Space wrapping
  through them. Each re-issues `launch {inspect:true, opts.stage}` for the same cast,
  which retires the running preview and re-enters inspection on that animation at
  frame 0; the brief's per-animation ◇ starts one the same way. **Close semantics:**
  every browser preview and every runtime scene whose cast includes the *player* is
  aborted when the browser closes; ordinary NPC-only scenes keep running
  (vignettes/machinima) and resurface in this list on the next open.
  Native→web `mode {mode:"wheel", tagPrefix, target:{token,name}|null}`
  switches the view into **animation-wheel mode** (see below); any other `mode`
  restores the console. Flash-free wheel opens rely on OSF UI **queuing
  messages sent to a not-yet-visible view and delivering them before its
  first paint** (C ABI MINOR ≥ 2): the DLL pushes the mode before
  `RequestMenu(open)`.
- The view becomes ready when the 2.0 helper's `ready` promise resolves, or when
  OSF Animation answers the catalog/version request that every page mount sends.
  The response path lets a manually reloaded web view self-heal after the host's
  ready lifecycle has already completed. It never gates on the
  protocol/version strings — the contract evolves additively. Platform pushes it consumes: `ui.visibility` (open/close relay,
  wheel-mode exit, orbit-drag reset) and `ui.error` (surfaced in the notice
  footer). **Gamepad:** the view requests OSF UI's `buttons` gamepad mode on
  `runtime.ready`. OSF UI maps D-pad/A/B (including D-pad hold-repeat) while
  leaving both sticks available to the native scene-orbit camera; the browser
  needs no raw-gamepad adapter or stick-event subscription. It also takes the
  `osfui.handleBack` grant so B arrives as Escape and can cancel an open wheel,
  settings panel, import panel, route debugger, or pick operation before closing
  the browser.
- **Targeting:** PICK arms actor or furniture selection, then a click in the
  transparent world area screen-tests loaded 3D bounds and returns the chosen
  reference; dragging more than five pixels remains camera orbit and Escape
  cancels. **Scan Nearby** remains the cell-walk fallback for crowded targets
  and invisible AI markers. The legacy open-time crosshair capture remains an
  additive native bridge command for older views. Its cell walk lists nearby actors (living, closest-first, with a species tag for creature
  filtering) and furniture with per-anchor scene counts, each as a clickable
  token. Scan rows draw a neutral silhouette (no portrait capture).
  The **Location** step places free-space scenes at Cast A (default), the player,
  a selected cast member, ten feet in front of the player, or a picked
  furniture/AI-marker reference. `launch` carries this as
  `location:{mode:"cast"|"player"|"actor"|"front"|"furniture",token?}`; the DLL
  resolves reference modes through the rendered world transform, including
  attached ship/interior frames. Furniture-authored scenes still require and
  validate compatible furniture—the free-space choices never bypass that gate.
- Catalog = OSF Animation's **live content registry** (the native compatibility name is
  `SceneRegistry`; this is not a disk scan). The browser
  projects it into one player-facing catalog of **playables**: a library stage is an
  Animation, a `player.emote.*` definition is an Emote, and an ordinary authored
  definition is a Scene. Pack/folder/set names are collections only and never launch.
  The All / Animations / Emotes / Scenes controls are facets over that one catalog.
  Every entry carries `pack` (the file-level content-pack label, "" if unauthored) and
  `sourceFile` (the scene file's name, no directories); rows group into collapsible
  collection blocks keyed on `pack` → `sourceFile` → id-prefix fallback. Selecting or
  directly playing a library row launches exactly its referenced stage; it cannot walk
  into another animation in the collection. Generated raw clips extracted from authored
  scenes are author-detail/debug material and stay out of the normal catalog.
  The view only ever holds opaque integer **tokens** (player = `-1`), which
  the DLL maps back to `RE::*` refs and re-validates on the main thread
  before use.
- **Animation clean tier:** Browse defaults to poses & loopable clips
  only — stages tagged `transition`/`partial` (the vanilla dump's connective
  tissue) hide behind a "full library" banner toggle, and groups order
  photomode/pose sets first. The tier is bypassed while furniture is keyed
  (the anchor match already curates, and e.g. dance flavor clips are tagged
  `transition` upstream yet are the good content) and while searching (stage
  names are in the search hay — a hit must be visible).
- **Durations:** each stage card carries `loopSec` (clip loop length),
  `timerSec`/`loops` (stage timing), `openEnded` and `estSec`; each scene carries
  `estSec` (sum of stage estimates, holds counted as 2 loops), `estPartial`
  (some stage unmeasured — read "at least") and `openEnded`. All are `null`
  until the DLL's background clip probe has values
  (`Serialization/ClipDurations`, persisted in `<Documents>\My Games\Starfield\OSF\`);
  the DLL re-pushes `catalog.data` unsolicited when the scan lands, so the
  view must tolerate a second catalog push. The view renders `~2:30`, `+`, `∞`.

## Deployment (VFS merge — no copy in OSF UI)

OSF UI resolves its view dir relative to its own DLL
(`<DLL dir>\OSFUI\views\`), which under MO2 is the **virtual**
`Data\SFSE\Plugins\OSFUI\views\` — merged across all enabled mods. So this view
ships from OSF Animation's own mod folder; **no copy lives in the OSF UI repo or
mod**:

- `xmake` (`after_build`) deploys the generated production build to
  `MO2\mods\OSF Animation\SFSE\Plugins\OSFUI\views\osf.animation\browser\`
  (the two-level `views/<modId>/<viewName>/` layout OSF UI discovers).
- The DLL registers the view at runtime via `RegisterView("osf.animation/browser")`
  (C ABI 1.5) — the user's `config.json` is never edited. On an older OSF UI
  without `RegisterView`, the view only opens if the user lists it in `views`
  themselves (the DLL logs a warning).

Caveat: the merge relies on MO2's USVFS; a non-MO2 (real loose files) install
would need the folder placed next to `OSFUI.dll` manually.

## Settings / hotkeys

No drop-in `settings/osf.animation.json` file: the DLL registers the same
schema document at runtime (`RegisterSettingsSchema`, see
`src/API/UISettings.cpp`) — hotkeys (`hotkeys.openBrowser` and
`hotkeys.openWheel`, both unbound by default), interface preferences, scene launch
defaults, and log level all live in OSF UI's in-game settings menu under
**OSF Animation**. The browser's header gear opens a dedicated view of its own
preferences using the same `settings.get` / `settings.set` store, so changes made
there and in OSF UI's settings card stay synchronized. It covers after-launch
behavior, the opening tab, session browsing memory, library detail/source,
unavailable-scene visibility, default apparel/input-lock/camera/speed overrides, and
author-facing catalog details. World references such as selected actors and
furniture are deliberately never persisted.

## Animation wheel (transient mode)

The `openWheel` hotkey verb (native `API::OpenWheel`) opens this same view in a
radial **wheel mode**: `osf.animation.mode {mode:"wheel", tagPrefix, target}`
hides the console/brief and rings up to 12 solo, free-space animations; the ring's
ellipse is count-adaptive — near-circular for a handful, widening as it fills.
Before customization the ordered pool is derived from installed `player.emote.*` defaults, so it works out of
the box. Any free-space, single-human animation stage in the Animation Browser can be added.
The first **Add to Wheel**, removal, or reorder
materializes that whole default pool before applying the edit—customizing one
entry never makes every other default disappear. The explicit loadout persists
DLL-side in `<Documents>\My Games\Starfield\OSF\wheel-pins.json`, account-global,
surviving ReloadPacks and reinstalls. It is an ordered JSON array of minimal
`{"scene":"...","stage":0}` launch references (stage is omitted for a whole default Emote);
deleting it restores installed defaults, while [] is an intentionally empty wheel.
Each eligible animation or Emote row offers add/remove controls; the brief also
offers **Reset Defaults**. Quick Access membership shows as ◆ on its Browse row.
The hub names who plays—the crosshair target captured at open
time ("→ Sarah") or "You".
Arrows/hover step the ring, Enter/click launches (`osf.animation.launch` with
`castTokens:[token]` and the saved stage), success sends `osf.animation.requestClose`; a
launch error shows in the hub and the wheel stays open. Cancel = Esc,
right-click, or hub click. Exit is host-driven: the `ui.visibility` hide relay
clears wheel mode, so a later browser open always shows the normal console.

## Standalone dev

The page detects a missing bridge and runs standalone, so you can iterate
layout/logic in a normal browser. Run `npm run dev` from `ui/animation-browser`
and open `http://localhost:8791/__osfui/`. The OSF UI CLI harness owns the
game-sized frame, resolution controls, transparency checker, visibility and
locale simulation, bridge traffic, and HMR. The view uses its richer stateful
animation simulator inside the harness iframe.

**Live data:** `fixtures/live/{catalog,library}.json` are committed snapshot fixtures of
the payloads the DLL sends the in-game view. The standalone page resolves them
relative to its own module URL (through Vite's `/@fs/` route), preferring a
git-ignored `*.local.json` override. It loads them instead of the
mock catalog — status reads `standalone · live snapshot`. `library.json` (the
vanilla-packs lane) is generated offline by
`python tools/generate-library-snapshot.py`, which replicates the DLL's
`BuildCatalog(library)` over `dist/OSF` — re-run it after regenerating packs.
`catalog.json` is a one-time in-game dump (the runtime mirror code has since
been removed; est times for hand-authored packs come from the in-game probe, so
refreshing it would need a temporary re-add of that dump or hand-editing).
For a **full real-world catalog** instead, run
`python tools/generate-catalog-snapshot.py`: it models the scenes lane from
this repo's `dist/OSF` **plus the sibling `OSF Compatibility Packs` repo's
generated outputs** (extra directories as arguments), writing the git-ignored
`fixtures/live/catalog.local.json` — standalone loading prefers any
`<name>.local.json` override over the committed fixture, so the standalone
page then browses the real Gergel Ebanex / Snu Snu install (great for testing
grouping and layout at scale). Delete the file to fall back to the committed
dump. Durations the in-game probe would supply come out `null`.
Pick/scan/launch stay stubbed (they need live refs). With no snapshot (or when
opened via `file://`), it falls back to the built-in mock catalog. These fixtures do NOT ship in-game: they live outside the generated output copied by `xmake`.

To exercise the **animation wheel** standalone: press `W` (mock crosshair target) or
`Shift+W` (player-only), or call `window.mockOpenWheel(withTarget)` from the
console; add `?wheel` to the iframe URL (`?wheel=solo` for no target)
boots straight into wheel mode so a plain reload keeps you there. The mock
catalog carries 14 `player.emote.*` quick emotes so the hard 12-entry cap is exercised;
picking **Facepalm** mock-fails to exercise the error path, any other pick
"launches" and closes the wheel via the mocked `osf.animation.requestClose` →
`ui.visibility` hide round-trip.

**Backdrop:** in-game the page body is transparent over the live game world; a
desktop browser renders that as flat white/black, which lies about contrast. In
standalone mode a **dev backdrop** stands a fake world behind the overlay —
procedural scenes cycled with `B` (dark ship **interior** → bright **day**, the
readability worst case → **night** exterior → flat **none**), sticky per tab.
Drop a real screenshot at `ui/animation-browser/fixtures/live/backdrop.jpg` (git-ignored, never packaged) and
it joins the cycle as **shot** and becomes the default.

While the wheel is up, a **WHEEL DEBUG strip** (top-left, standalone only —
injected only when no bridge exists, so it can never surface in-game) drives
every wheel state without in-game round-trips: `−`/`+` step a generated emote
pool through wheelGeom's whole range (0 = the empty state, 1–3 = the tight
ring, past 12 proves the hard cap, and emotes cycle with numbered titles past 14);
`PINS×3` pins the first three emotes in *reverse* order to prove pin-order
sorting; `TARGET` flips the hub between "→ Sarah Morgan" and "You"; `ERROR`
plants a hub launch error; `LOADING` shows the catalog-pending state; `RESET`
returns to the real (snapshot/mock) catalog.

**Harness toolbar:** `osfui.mock.ts` contributes the view's non-wheel debug
switches to the harness shell toolbar instead of overlaying them on the layout.
**OSF UI host** fakes the installed host version the status line reports — *no
host info* (the default), *host up to date*, or *host older than tested*, which
raises the amber `UPDATE` badge and its Nexus link. Clicks reach the view over a
window event (`src/dev/harness-tools.ts`); the mock module shares the view page.

**Loadout standalone:** the `wheel.set` round-trip is mocked with a session-local
ordered loadout applied on top of whichever catalog is served. Remove or reorder
an emote from its brief, then `W`: the wheel retains all other defaults in the
chosen order. **Reset Defaults** drops the explicit list and derives the installed
defaults again.

## Aesthetic

Starfield "NASA-punk" maintenance-HUD, aligned to the shared **OSF design system**
(burnt amber `--accent` + brushed steel on a near-black void, teal HUD signal):
one framed **console** with amber corner brackets and a faint scan grid, a **slate**
header (cast / anchor / readiness + debug toggle), three **bays** (READY NOW /
NEEDS ONE THING / LIBRARY) of spine-numbered scene cards with per-gate pips, and an
instrument **brief** module (registry id, requirements, seats, launch). Saira Semi
Condensed (Bahnschrift stand-in) for chrome, JetBrains Mono / Cascadia for data.
Restrained glows and gradients are intentional here — every glow signals state.

The design-system tokens are inlined into `src/styles/browser.css` (the view ships self-contained;
OSF UI serves no network, so webfonts fall back to Bahnschrift/Segoe). Source design:
Claude Design "Scene Director" wireframe.
