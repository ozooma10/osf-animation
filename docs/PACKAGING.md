# Packaging OSF Animation

The engine and the optional browser/wheel now have separate release archives. Both are built from this repository and share the version in `xmake.lua`.

```powershell
packaging\build-archive.ps1
packaging\build-browser-addon.ps1
```

`build-archive.ps1` configures `releasedbg`, builds the native DLL, stages the Papyrus API and internal scene, verifies the payload, and writes `packaging/out/OSF Animation v<version>.zip`. It deploys the engine to the configured MO2 mod folder through xmake. `-SkipBuild`, `-CompilePapyrus`, `-IncludePdb`, `-Version`, and `-Tag` remain available.

`build-browser-addon.ps1` runs `tools/BuildBrowserAddon.ps1`, which installs npm dependencies, runs the browser tests/typecheck/build, and deploys to a separate MO2 folder when `XSE_SF_MODS_PATH` is set. It stages the web view and the optional vanilla library and emote content as a FOMOD, then writes `packaging/out/OSF Animation Browser v<version>.zip`. `-SkipBuild`, `-Version`, and `-Tag` are available.

The engine archive installs `OSF Animation.dll`, `OSF`/`OSFTypes`/`OSFAdvanced` `.pex` and `.psc` files, `OSF/internal.osf.json`, the Settings schema, and license documents. It contains no browser view or vanilla/emote pack. The addon installs the browser view and can optionally install the library and emote pack; it depends on OSF Animation, OSF UI, and OSF Settings.

The existing browser bridge, browser preferences, and hotkey registration still live in the engine DLL. This packaging split removes the web build and web assets from the native build and core installation. Extracting those native parts requires a versioned catalog/inspection interface so the addon can use the engine without including its private registries and preview types. Until then, ship the engine and addon from matching versions and keep the released `OSF.OpenBrowser()` Papyrus entry compatible.

Neither archive includes player settings, source maps, the developer-only `OSFTest` Papyrus script, nor the browser's source and `node_modules`.
