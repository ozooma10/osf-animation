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
  `projectPickables`→`pickTargets`, `pickScreen`→`pick`, `scanNearby`→`scanResults`,
  `anchorMatch`→`anchorMatch`, `launch`→`launchResult`, `stop`, `advance`, and
  `playback.get`/`playback.set {handle,paused}` for active runtime status and pause/resume.
  A launch may carry additive `singleAnimation:true`; after entering the requested `opts.stage`,
  any scene edge out ends playback instead of continuing through the parent definition.
  `orbit` steers the native camera; `opened`/`closed` report visibility; and `requestClose`
  asks the host to hide the view. Native→web `activeScenes` carries live scene handles, stage,
  cast, clock, duration, and speed for the ACTIVE tab and compact running controls. Runtime
  playback is forward-only; the browser can pause/resume, advance, or stop it.
- The view becomes ready when the 2.0 helper's `ready` promise resolves, or when
  OSF Animation answers the catalog/version request that every page mount sends.
  The response path lets a manually reloaded web view self-heal after the host's
  ready lifecycle has already completed. It never gates on the
  protocol/version strings — the contract evolves additively. Platform pushes it consumes: `ui.visibility` (open/close relay
  and orbit-drag reset) and `ui.error` (surfaced in the notice
  footer). **Gamepad:** the view takes the `osfui.gamepadRaw` grant on
  `runtime.ready` — the runtime's default mapping would route both sticks
  into UI nav/scroll, but the sticks belong to the native scene-orbit camera
  (the DLL polls XInput directly). The PAD NAV layer re-creates the button
  half from raw `ui.gamepad` events: D-pad → arrows (hold-repeat), A → Enter,
  B → close; stick events are dropped on purpose.
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
  into another animation in the collection.
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
  `src/API/UISettings.cpp`) — the unbound `hotkeys.openBrowser` hotkey,
  interface preferences, scene launch
defaults, and log level all live in OSF UI's in-game settings menu under
**OSF Animation**. The browser's header gear opens a dedicated view of its own
preferences using the same `settings.get` / `settings.set` store, so changes made
there and in OSF UI's settings card stay synchronized. It covers after-launch
behavior, the opening tab, session browsing memory, library detail/source,
unavailable-scene visibility, default apparel/input-lock/camera/speed overrides, and
author-facing catalog details. World references such as selected actors and
furniture are deliberately never persisted.

## Standalone dev

The page detects a missing bridge and runs standalone, so you can iterate
layout/logic in a normal browser. Run `npm run dev` from `ui/animation-browser`
and open `http://localhost:8791/__osfui/`. The OSF UI CLI harness owns the
game-sized frame, resolution controls, transparency checker, visibility and
locale simulation, bridge traffic, and HMR. The view uses a stateful runtime-scene simulator inside the harness iframe.

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

**Backdrop:** in-game the page body is transparent over the live game world; a
desktop browser renders that as flat white/black, which lies about contrast. In
standalone mode a **dev backdrop** stands a fake world behind the overlay —
procedural scenes cycled with `B` (dark ship **interior** → bright **day**, the
readability worst case → **night** exterior → flat **none**), sticky per tab.
Drop a real screenshot at `ui/animation-browser/fixtures/live/backdrop.jpg` (git-ignored, never packaged) and
it joins the cycle as **shot** and becomes the default.

**Harness toolbar:** `osfui.mock.ts` contributes the view's debug
switches to the harness shell toolbar instead of overlaying them on the layout.
**OSF UI host** fakes the installed host version the status line reports — *no
host info* (the default), *host up to date*, or *host older than tested*, which
raises the amber `UPDATE` badge and its Nexus link. Clicks reach the view over a
window event (`src/dev/harness-tools.ts`); the mock module shares the view page.

## Aesthetic

Starfield "NASA-punk" maintenance-HUD, aligned to the shared **OSF design system**
(burnt amber `--accent` + brushed steel on a near-black void, teal HUD signal):
one framed **console** with amber corner brackets and a faint scan grid, a **slate**
header (cast / anchor / readiness + settings), three **bays** (READY NOW /
NEEDS ONE THING / LIBRARY) of spine-numbered scene cards with per-gate pips, and an
instrument **brief** module (registry id, requirements, seats, launch). Saira Semi
Condensed (Bahnschrift stand-in) for chrome, JetBrains Mono / Cascadia for data.
Restrained glows and gradients are intentional here — every glow signals state.

The design-system tokens are inlined into `src/styles/browser.css` (the view ships self-contained;
OSF UI serves no network, so webfonts fall back to Bahnschrift/Segoe). Source design:
Claude Design "Scene Director" wireframe.
