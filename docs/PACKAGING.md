# Packaging OSF Animation

The engine and the optional browser/wheel now have separate release archives. Both are built from this repository and share the version in `xmake.lua`.

```powershell
packaging\build-archive.ps1
packaging\build-browser-addon.ps1
```

`build-archive.ps1` configures production `releasedbg` with `animation_smoke=n`, builds the native DLL, stages the Papyrus API and internal scene, verifies the payload, and writes `packaging/out/OSF Animation v<version>.zip`. Build/install output stays in private staging under `build/packages`; packaging does not deploy to MO2. `-SkipBuild`, `-CompilePapyrus`, `-IncludePdb`, `-Version`, `-Tag`, and `-ResultPath` are available. The version must match `xmake.lua`.

`build-browser-addon.ps1` runs `tools/BuildBrowserAddon.ps1 -NoDeploy`, which installs npm dependencies and runs the browser tests/typecheck/build. It stages the web view and the optional vanilla library and emote content as a FOMOD, then writes `packaging/out/OSF Animation Browser v<version>.zip`. `-SkipBuild`, `-Version`, `-Tag`, and `-ResultPath` are available. To deploy development builds, use ordinary `xmake` for the engine and `tools/BuildBrowserAddon.ps1` for the browser.

Both packagers require PowerShell 7.2+. Builds record source fingerprints (including dirty files and submodules), released SDK provenance, configuration, and output hashes in `build/engine-receipt.json` or `build/browser-receipt.json`. `-SkipBuild` rejects missing receipts, changed source, changed artifacts, and instrumented DLLs. Ordinary `xmake` and `npm run build` do not issue release receipts; use `tools/BuildProduction.ps1`, `tools/BuildBrowserAddon.ps1 -NoDeploy`, or the smoke bench.

The engine packager rejects a PSC newer than its PEX and compares all compiled native signatures with the shipped PSC declarations and C++ registration names. Use `-CompilePapyrus` to refresh the scripts before building. These checks do not establish VM binding or `OSFTypes` struct layout in a running game.

Each ZIP has a `.sha256` and `.manifest.json` companion. The manifest records the exact entry hashes and build receipt; the ZIP is reopened and every entry checked before publication to the final path. `-ResultPath <file>` writes the absolute manifest path for automation. Staging directories are retained for inspection. No archive is uploaded by these scripts.

The engine archive installs `OSF Animation.dll`, `OSF`/`OSFTypes`/`OSFAdvanced` `.pex` and `.psc` files, `OSF/internal.osf.json`, the Settings schema, and license documents. It contains no browser view or vanilla/emote pack. The addon installs the browser view and can optionally install the library and emote pack; it depends on OSF Animation, OSF UI, and OSF Settings.

The existing browser bridge, browser preferences, and hotkey registration still live in the engine DLL. This packaging split removes the web build and web assets from the native build and core installation. Extracting those native parts requires a versioned catalog/inspection interface so the addon can use the engine without including its private registries and preview types. Until then, ship the engine and addon from matching versions and keep the released `OSF.OpenBrowser()` Papyrus entry compatible.

Neither archive includes player settings, source maps, the developer-only `OSFTest` Papyrus script, nor the browser's source and `node_modules`.
