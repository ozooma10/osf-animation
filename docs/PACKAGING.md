# Packaging OSF Animation for release

How the downloadable mod archive is assembled. The build deploys loose files to
`MO2\mods\OSF Animation\` for dev; this doc + `packaging/` turn those into a distributable
**FOMOD archive** for LoversLab / Nexus.

## Dependencies (state these on the mod page — not bundled)

- **Starfield 1.16.244.0** (the verified build; the DLL self-disables on a mismatch).
- **SFSE** matching that game version.
- **Address Library for SFSE Plugins** — OSF resolves engine addresses through it. *Not bundled*
  (it's a separately-maintained mod); list it as a hard requirement.

> There is **no `versionlib-*.bin` to ship** — OSF does not carry its own; it uses the Address
> Library mod above. (The workspace's local AddressLib mod folder is unrelated to this package.)

## What ships vs. what doesn't

Built from the repo (`build/` for the DLL, `dist/` for scripts + content), **not** from the MO2
deploy folder (which accumulates dev junk like `settings.json` / `zzz_*` probes).

| Ships | Excluded |
|---|---|
| `OSF Animation.dll` | `OSF Animation.pdb` (debug symbols — never ship) |
| `OSF`/`OSFCompat`/`OSFEvent` `.pex` + `.psc` | `OSFTest.pex`/`.psc` (console test harness → Examples only) |
| `SAF`/`SAFScript` `.pex` + `.psc` (shim) | the test `*.scene.json` + `OSFTestPack.json` (→ Examples only) |
| | `OSF/Animations/OSF_Test/*.glb` (test clips → Examples only) |
| | `settings.json`, `zzz_flicker_probe.json`, any dev probe |

> **DLL flavor:** the verified, in-game-tested build is **`releasedbg`** (`build/windows/x64/
> releasedbg/`). The bare `release` flavor has a known config footgun for this project (wrong-flavor
> DLL / stale pdb — see the build-mode memory). Ship `releasedbg` unless `release` has been
> rebuilt+verified. Either way, **omit the `.pdb`**.

> **Constants:** there is no `OSFConst` companion script. It was compiler-tested and rejected because
> Papyrus cannot read properties directly on a type name. The canonical constant form is
> `OSF.EVENT_X()` / `OSF.RESULT_X()`.

## Archive layout (FOMOD)

```text
OSF Animation v<ver>.zip
├── fomod/
│   ├── info.xml            ← metadata (name, author, version)
│   └── ModuleConfig.xml    ← the installer: required Core + optional groups
├── Core/                   ← REQUIRED (installed to Data/)
│   ├── SFSE/Plugins/OSF Animation.dll
│   ├── Scripts/OSF.pex, OSFCompat.pex, OSFEvent.pex
│   └── Scripts/Source/OSF.psc, OSFCompat.psc, OSFEvent.psc
├── SAF/                    ← RECOMMENDED group "SAF compatibility shim"
│   ├── Scripts/SAF.pex, SAFScript.pex
│   └── Scripts/Source/SAF.psc, SAFScript.psc
└── Examples/              ← OPTIONAL group "Example scenes & test pack" (off by default)
    ├── OSF/OSFTestPack.json, *.scene.json
    ├── OSF/Animations/OSF_Test/*.glb
    └── Scripts/OSFTest.pex, Scripts/Source/OSFTest.psc
```

Each top-level folder is a FOMOD *folder install* with `destination=""`, so its internal
`SFSE/…`, `Scripts/…`, `OSF/…` structure lands at the Data root.

## FOMOD components (the install choices)

1. **Core (required, always installed)** — the DLL + the `OSF*` Papyrus scripts (and their sources,
   which a consumer mod needs in its compiler import path). This is the framework.
2. **SAF compatibility shim (recommended, pre-checked)** — the `SAF`/`SAFScript` scripts so existing
   SAF-dependent mods run unchanged (the migration headline). Harmless if unused; recommend on.
3. **Example scenes & test pack (optional, unchecked)** — demo `*.scene.json`, the baked-in test
   animation pack, and the `OSFTest` console harness. For learning the scene-file format or smoke-
   testing the install; not needed for normal use.

A non-FOMOD-aware manager (or a manual install) that just extracts the archive gets Core + SAF +
Examples all at once — still valid, just without the opt-out.

## Building the archive

```powershell
# from the repo root, after a verified build (xmake) :
packaging\build-archive.ps1 -Version 0.1.0-beta
# -> packaging\out\OSF Animation v0.1.0-beta.zip
```

The script (`packaging/build-archive.ps1`) stages `Core/`, `SAF/`, `Examples/` from `build/` + `dist/`
(filtering out test/dev files for Core/SAF), copies `packaging/fomod/`, stamps the version into
`info.xml`, and zips. Re-run after every build you intend to publish.

## Pre-publish checklist

- [ ] **Confirm the version** — `xmake.lua` `set_version("…")` is currently `0.1.0`; rebuild so
      `OSF.GetVersion()` matches the archive name you publish.
- [ ] Rebuild + **verify in-game** (the `releasedbg` DLL hash matches the one you tested).
- [ ] Confirm the archive contains **no `.pdb`**, no `settings.json`/`zzz_*`, no test content in Core.
- [ ] Install the FOMOD in a clean MO2 profile; verify Core-only, Core+SAF, and Core+SAF+Examples.
- [ ] Mod page lists the three dependencies above + the "uninstall SAF first" note (SAF_MIGRATION.md).
