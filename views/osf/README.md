# OSF Animation — scene director view

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
  `osf.launch`→`osf.launchResult`, `osf.stop`.
  The view gates its JS on `runtime.ready.bridgeVersion === "0.1"`.
- **Targeting:** crosshair pick *or* **Scan Nearby** — a `parentCell` walk that
  lists nearby actors (living, closest-first) and furniture *usable by the
  selected scene* (via the anchor matcher), each as a clickable token.
- Catalog = OSF Animation's **live** `SceneRegistry` (not a disk scan).
  Targets are crosshair-picked; the view only ever holds opaque integer
  **tokens** (player = `-1`), which the DLL maps back to `RE::*` refs and
  re-validates on the main thread before use.

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

## Standalone dev

The page detects a missing bridge and runs on a mock catalog, so you can iterate
layout/logic in a normal browser. A preview server is configured in
`.claude/launch.json` (`python -m http.server`, serves this folder).

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
