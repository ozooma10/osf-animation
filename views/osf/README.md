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
  `osf.pickCrosshair`→`osf.pick`, `osf.launch`→`osf.launchResult`, `osf.stop`.
  The view gates its JS on `runtime.ready.bridgeVersion === "0.1"`.
- Catalog = OSF Animation's **live** `SceneRegistry` (not a disk scan).
  Targets are crosshair-picked; the view only ever holds opaque integer
  **tokens** (player = `-1`), which the DLL maps back to `RE::*` refs and
  re-validates on the main thread before use.

## Deployment (v1 disk-drop)

OSF UI (v1) loads views only from its **own** data dir, so this folder is the
authored source and a copy is shipped into OSF UI:

- `C:\Modding\Starfield\OSF UI\data\OSFUI\views\osf\`  (OSF UI repo, deploys with OSF UI)
- registered in `data\OSFUI\config.json`: `"view": "osf"`, `views` includes `"osf"`.

Both are mirrored into the live `MO2\mods\OSF UI\SFSE\Plugins\OSFUI\...` deploy.

> **v1.1:** OSF UI's planned `RegisterViewRoot` will let OSF Animation ship this
> view under its **own** mod folder, removing the OSF UI copy. Until then keep
> the two in sync (they are identical files).

## Standalone dev

The page detects a missing bridge and runs on a mock catalog, so you can iterate
layout/logic in a normal browser. A preview server is configured in
`.claude/launch.json` (`python -m http.server`, serves this folder).

## Aesthetic

Starfield "NASA-punk": burnt orange + bone on warm near-black, DIN-ish sans for
chrome, monospace for data, instrument panels with corner ticks. Amber armature
glyph = the Animation module accent in the OSF family. No gradients/glow.
