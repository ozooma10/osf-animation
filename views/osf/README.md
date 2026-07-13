# OSF Animation — scene browser view

The in-game MVP scene browser/launcher. It is an HTML/CSS/JS view rendered by
**OSF UI** (Ultralight overlay) and driven **natively** by OSF Animation's own
DLL over OSF UI's bridge API. Only JSON text crosses the boundary.

## How it's wired

```
views/osf/ (this folder)  ──►  OSF UI  MessageBridge  ──►  OSF Animation DLL
   window.osfui.postMessage        (ui.command)            src/API/UIBridge.cpp
   window.osfui.onMessage      ◄── SendToWeb ──────────────  osf.* handlers
```

- **Native side:** `src/API/UIBridge.{h,cpp}` (vendored bridge header
  `src/API/OSFUI_API.h`). Registered in `src/main.cpp` at `kPostDataLoad`
  (`OSF::API::InstallUIBridge()`), a no-op when OSF UI is absent.
- **Contract (`osf.*`):** `osf.catalog.get`→`osf.catalog.data`,
  `osf.pickCrosshair`→`osf.pick`, `osf.scanNearby`→`osf.scanResults`,
  `osf.launch`→`osf.launchResult`, `osf.stop`, `osf.requestClose` (view asks the
  host to hide it — the view can't close itself; used by the emote wheel).
  Native→web `osf.mode {mode:"wheel", tagPrefix, target:{token,name}|null}`
  switches the view into **emote-wheel mode** (see below); any other `mode`
  restores the console. The DLL pushes a mode on **every** open (`osf.opened` →
  wheel|browser): the view veils its console from `ui.visibility` show until
  that push lands, so a wheel open never flashes the browser UI (500 ms
  fallback timeout covers a DLL too old to push modes).
  `osf.portraits.get {tokens}`→`osf.portrait.data {portraits:[{formId,dataUri}]}` —
  actor headshots for the scan list as PNG data URIs; cached ones return in one
  batch, queued captures land later as unsolicited single-item pushes (the view
  renders a silhouette placeholder until then, upgrading rows in place).
  The view gates its JS on `runtime.ready.bridgeVersion === "0.1"`.
- **Targeting:** crosshair pick *or* **Scan Nearby** — a `parentCell` walk that
  lists nearby actors (living, closest-first) and furniture *usable by the
  selected scene* (via the anchor matcher), each as a clickable token.
- Catalog = OSF Animation's **live** `SceneRegistry` (not a disk scan).
  Targets are crosshair-picked; the view only ever holds opaque integer
  **tokens** (player = `-1`), which the DLL maps back to `RE::*` refs and
  re-validates on the main thread before use.
- **Durations:** each stage card carries `loopSec` (clip loop length),
  `timerSec`/`loops` (stage timing), `openEnded` and `estSec`; each scene carries
  `estSec` (sum of stage estimates, holds counted as 2 loops), `estPartial`
  (some stage unmeasured — read "at least") and `openEnded`. All are `null`
  until the DLL's background clip probe has values
  (`Serialization/ClipDurations`, persisted in `<Documents>\My Games\Starfield\OSF\`);
  the DLL re-pushes `osf.catalog.data` unsolicited when the scan lands, so the
  view must tolerate a second catalog push. The view renders `~2:30`, `+`, `∞`.

## Deployment (VFS merge — no copy in OSF UI)

OSF UI resolves its view dir relative to its own DLL
(`<DLL dir>\OSFUI\views\`), which under MO2 is the **virtual**
`Data\SFSE\Plugins\OSFUI\views\` — merged across all enabled mods. So this view
ships from OSF Animation's own mod folder; **no copy lives in the OSF UI repo or
mod**:

- `xmake` (`after_build`) deploys this folder to
  `MO2\mods\OSF Animation\SFSE\Plugins\OSFUI\views\osf\`.
- OSF UI's `data\OSFUI\config.json` must still register it:
  `"view": "osf"`, `views` includes `"osf"` (view *membership* is OSF UI config).

Caveat: the merge relies on MO2's USVFS; a non-MO2 (real loose files) install
would need the folder placed next to `OSFUI.dll` manually.

## Emote wheel (transient mode)

The `openWheel` hotkey verb (native `API::OpenWheel`) opens this same view in a
radial **wheel mode**: `osf.mode {mode:"wheel", tagPrefix, target}` hides the
console/brief and rings up to 12 solo scenes whose tags start with `tagPrefix`
(default `player.emote.`; overflow shows "+N more"). The hub names who plays —
the crosshair target captured at open time ("→ Sarah") or "You". Arrows/hover
step the ring, Enter/click launches (`osf.launch` with `castTokens:[token]`, no
opts), success sends `osf.requestClose`; a launch error shows in the hub and the
wheel stays open. Cancel = Esc, right-click, or hub click. Exit is host-driven:
the `ui.visibility` hide relay clears wheel mode, so a later F10 open always
shows the normal console.

## Standalone dev

The page detects a missing bridge and runs on a mock catalog, so you can iterate
layout/logic in a normal browser. A preview server is configured in
`.claude/launch.json` (`python -m http.server`, serves this folder).

To exercise the **emote wheel** standalone: press `W` (mock crosshair target) or
`Shift+W` (player-only), or call `window.mockOpenWheel(withTarget)` from the
console. The mock catalog carries 14 `player.emote.*` scenes so the 12-slice cap
shows "+2 more"; picking **Facepalm** mock-fails to exercise the error path, any
other pick "launches" and closes the wheel via the mocked `osf.requestClose` →
`ui.visibility` hide round-trip.

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
