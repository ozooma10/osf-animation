# OSF Animation — scene browser view

The in-game scene browser/launcher. It is an HTML/CSS/JS view rendered by
**OSF UI** (Ultralight overlay) and driven **natively** by OSF Animation's own
DLL over OSF UI's bridge API (protocol **1.0**). Only JSON text crosses the
boundary.

## How it's wired

```
views/osf.animation/browser/ (this folder)  ──►  OSF UI  MessageBridge  ──►  OSF Animation DLL
   window.osfui.postMessage        (ui.command)              src/API/UIBridge.cpp
   window.osfui.onMessage      ◄── SendToWeb ────────────────  osf.animation.* handlers
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
  `pickCrosshair`→`pick`, `scanNearby`→`scanResults`,
  `anchorMatch`→`anchorMatch` (reply), `launch`→`launchResult`, `stop`,
  `orbit {dx,dy,wheel}` (world-drag steers the native orbit camera; no reply),
  `opened`/`closed` (visibility reports off the `ui.visibility` relay), and
  `requestClose` (view asks the host to hide it — used by the emote wheel).
  Native→web `mode {mode:"wheel", tagPrefix, target:{token,name}|null}`
  switches the view into **emote-wheel mode** (see below); any other `mode`
  restores the console. Flash-free wheel opens rely on OSF UI **queuing
  messages sent to a not-yet-visible view and delivering them before its
  first paint** (C ABI MINOR ≥ 2): the DLL pushes the mode before
  `RequestMenu(open)`.
- The view requires only that a bridge is present (`runtime.ready` arrives);
  it never gates on the protocol/version strings — the contract evolves
  additively. Platform pushes it consumes: `ui.visibility` (open/close relay,
  wheel-mode exit, orbit-drag reset) and `ui.error` (surfaced in the notice
  footer). Gamepad works through the runtime's default mapping (D-pad/left
  stick → arrows, A → Enter, B → close, right stick → scroll) feeding the
  view's directional-focus layer; no raw `ui.gamepad` handling.
- **Targeting:** crosshair pick *or* **Scan Nearby** — a cell walk that
  lists nearby actors (living, closest-first, with a species tag for creature
  filtering) and furniture with per-anchor scene counts, each as a clickable
  token. Scan rows draw a neutral silhouette (no portrait capture).
- Catalog = OSF Animation's **live** `SceneRegistry` (not a disk scan).
  The view only ever holds opaque integer **tokens** (player = `-1`), which
  the DLL maps back to `RE::*` refs and re-validates on the main thread
  before use.
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

- `xmake` (`after_build`) deploys this folder to
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
`src/API/UISettings.cpp`) — hotkeys (`hotkeys.openBrowser`,
`hotkeys.openWheel`, default unbound), interface toggles, and log level all
live in OSF UI's in-game settings menu under **OSF Animation**.

## Emote wheel (transient mode)

The `openWheel` hotkey verb (native `API::OpenWheel`) opens this same view in a
radial **wheel mode**: `osf.animation.mode {mode:"wheel", tagPrefix, target}`
hides the console/brief and rings up to 12 solo scenes whose tags start with
`tagPrefix` (default `player.emote.`; overflow shows "+N more"). The hub names
who plays — the crosshair target captured at open time ("→ Sarah") or "You".
Arrows/hover step the ring, Enter/click launches (`osf.animation.launch` with
`castTokens:[token]`, no opts), success sends `osf.animation.requestClose`; a
launch error shows in the hub and the wheel stays open. Cancel = Esc,
right-click, or hub click. Exit is host-driven: the `ui.visibility` hide relay
clears wheel mode, so a later browser open always shows the normal console.

## Standalone dev

The page detects a missing bridge and runs on a mock catalog, so you can iterate
layout/logic in a normal browser. A preview server is configured in
`.claude/launch.json` (`python -m http.server`, serves this folder).

To exercise the **emote wheel** standalone: press `W` (mock crosshair target) or
`Shift+W` (player-only), or call `window.mockOpenWheel(withTarget)` from the
console. The mock catalog carries 14 `player.emote.*` scenes so the 12-slice cap
shows "+2 more"; picking **Facepalm** mock-fails to exercise the error path, any
other pick "launches" and closes the wheel via the mocked
`osf.animation.requestClose` → `ui.visibility` hide round-trip.

## Aesthetic

Starfield "NASA-punk" maintenance-HUD, aligned to the shared **OSF design system**
(burnt amber `--accent` + brushed steel on a near-black void, teal HUD signal):
one framed **console** with amber corner brackets and a faint scan grid, a **slate**
header (cast / anchor / readiness + author toggle), three **bays** (READY NOW /
NEEDS ONE THING / LIBRARY) of spine-numbered scene cards with per-gate pips, and an
instrument **brief** module (registry id, requirements, seats, launch). Saira Semi
Condensed (Bahnschrift stand-in) for chrome, JetBrains Mono / Cascadia for data.
Restrained glows and gradients are intentional here — every glow signals state.

The design-system tokens are inlined into `style.css` (the view ships self-contained;
OSF UI serves no network, so webfonts fall back to Bahnschrift/Segoe). Source design:
Claude Design "Scene Director" wireframe.
