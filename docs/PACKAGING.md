# Packaging OSF Animation

The engine and the optional browser/wheel now have separate release archives. Both are built from this repository and share the version in `xmake.lua`.
Browser sources and installer definitions live in `extensions/browser/`; the root `tools/` and
`packaging/` scripts remain the build, deployment, and release entry points.

```powershell
packaging\build-archive.ps1
packaging\build-browser-addon.ps1
```

`build-archive.ps1` configures production `releasedbg` with `animation_smoke=n`, builds the native DLL, stages the Papyrus API and internal scene, verifies the payload, and writes `packaging/out/OSF Animation v<version>.zip`. Build/install output stays in private staging under `build/packages`; packaging does not deploy to MO2. `-SkipBuild`, `-CompilePapyrus`, `-IncludePdb`, `-Version`, `-Tag`, and `-ResultPath` are available. The version must match `xmake.lua`.

`build-browser-addon.ps1` runs `tools/BuildBrowserAddon.ps1 -NoDeploy`, which builds the separate production `OSF Animation Browser.dll`, installs npm dependencies, and runs the browser tests/typecheck/build. It stages the addon DLL and web view and the optional vanilla library and emote content as a FOMOD, then writes `packaging/out/OSF Animation Browser v<version>.zip`. `-SkipBuild`, `-Version`, `-Tag`, and `-ResultPath` are available. To deploy development builds, use ordinary `xmake` for the engine and `tools/BuildBrowserAddon.ps1` for the browser.

Both packagers require PowerShell 7.2+. Builds record source fingerprints (including dirty files and submodules), released SDK provenance, configuration, and output hashes in `build/engine-receipt.json` or `build/browser-receipt.json`. `-SkipBuild` rejects missing receipts, changed source, changed artifacts, and instrumented DLLs. Ordinary `xmake` and `npm run build` do not issue release receipts; use `tools/BuildProduction.ps1`, `tools/BuildBrowserAddon.ps1 -NoDeploy`, or the smoke bench.

The engine packager rejects a PSC newer than its PEX and compares all compiled native signatures with the shipped PSC declarations and C++ registration names. Use `-CompilePapyrus` to refresh the scripts before building. These checks do not establish VM binding or `OSFTypes` struct layout in a running game.

Each ZIP has a `.sha256` and `.manifest.json` companion. The manifest records the exact entry hashes and build receipt; the ZIP is reopened and every entry checked before publication to the final path. `-ResultPath <file>` writes the absolute manifest path for automation. Staging directories are retained for inspection. No archive is uploaded by these scripts.

The browser receipt binds the DLL, browser schema, and all generated view files together; neither can change after verification. `Test-BrowserBoundary.ps1` rejects private engine includes/linkage. No engine DLL or Papyrus files belong in the addon ZIP.

The engine archive installs `OSF Animation.dll`, `OSF`/`OSFTypes`/`OSFAdvanced` `.pex` and `.psc` files, `OSF/internal.osf.json`, the Settings schema, and license documents. It contains no browser view or vanilla/emote pack. The addon installs `OSF Animation Browser.dll`, the browser view and can optionally install the library and emote pack; it depends on OSF Animation, OSF UI, and OSF Settings.

The native bridge, world picking, browser sessions, wheel persistence, preferences, and hotkey registration live in the addon DLL. It consumes scene API 1.3 and engine services API 2.0, with no private engine sources or import library. `OSF.OpenBrowser()` is an optional forwarder. Core ships only `osf.animation.engine.json`; Browser ships `osf.animation.browser.json`. Each imports its own saved preferences from the legacy Settings ID once; browser hotkeys need rebinding after the ID change. Replace the old engine/browser mods on upgrade so the obsolete `osf.animation.json` schema is removed. See [BROWSER_ADDON.md](BROWSER_ADDON.md) for the boundary and lifetimes.

Neither archive includes player settings, source maps, the developer-only `OSFTest` Papyrus script, nor the browser's source and `node_modules`.
