# Packaging OSF Animation for release

`packaging/build-archive.ps1` builds OSF Animation and produces a FOMOD release archive under `packaging/out/`. It follows the same pipeline shape as OSF UI's `tools/package.ps1` (build → stage → verify → zip + SHA-256), with one structural difference: OSF Animation ships a **FOMOD installer** because it has opt-in content groups, so the archive root is `Core/ Library/ Immersion/ fomod/` rather than a bare `Data`-shaped tree.

## Quick start

```powershell
# Full release build (releasedbg) -> packaging\out\OSF Animation v1.0.0.zip
# (version comes from set_version(...) in xmake.lua)
packaging\build-archive.ps1

# Custom version / tag
packaging\build-archive.ps1 -Version 1.1.0 -Tag beta

# Package the current build without rebuilding
packaging\build-archive.ps1 -SkipBuild

# Recompile the Papyrus API with the CK compiler first (hard-fails on compile errors)
packaging\build-archive.ps1 -CompilePapyrus

# Ship the .pdb for crash-log symbolication (excluded by default)
packaging\build-archive.ps1 -IncludePdb
```

## What it does

1. **Configure + build** — `xmake f -m releasedbg -P .` + `xmake -P .` (the mode is pinned; a bare reconfigure silently drops it). The build's `before_build` hook rebuilds the browser view from `ui/animation-browser` into `build/views/` if its sources are newer than the last Vite output, so a packaged archive can't ship a stale UI. The view is generated, never committed — npm is required. `-SkipBuild` packages whatever is already built, browser view included.
2. **Papyrus surface check** — `OSF` / `OSFTypes` / `OSFAdvanced` `.pex` + `.psc` must exist (hard fail — consumers compile against the sources). A `.psc` newer than its `.pex` warns (stale `.pex` = new natives fail to bind at link time); `-CompilePapyrus` recompiles with the CK compiler instead. `OSFTest` is dev-only and must never ship — the script fails if it leaks into the stage.
3. **Stage the FOMOD tree** from the authoritative sources (`build/` DLL + generated browser view, `dist/` scripts + scene packs):
4. **License docs** — `LICENSE`, `EXCEPTIONS`, `THIRD_PARTY.md` go inside `SFSE/Plugins/OSF Animation/` so the game's `Data` root stays clean.
5. **Verify** — hard-fails on any missing required file, on an incomplete animation library (< 20 packs), and on any `<folder source>` in `ModuleConfig.xml` that doesn't exist in the stage.
6. **Zip + report** — writes `packaging/out/OSF Animation v<version>[-tag].zip`, prints size, file count, and SHA-256. The version (with tag) is also stamped into `fomod/info.xml`.

## Archive layout

```
OSF Animation v1.0.0.zip
├─ fomod/                              (installer: ModuleConfig.xml, info.xml, images/)
├─ Core/                               (required install)
│  ├─ SFSE/Plugins/
│  │  ├─ OSF Animation.dll             (+ .pdb only with -IncludePdb)
│  │  ├─ OSF Animation/                (LICENSE, EXCEPTIONS, THIRD_PARTY.md)
│  │  └─ OSFUI/views/osf.animation/browser/   (scene-browser view; OSF UI discovers it via VFS merge)
│  ├─ Scripts/{OSF,OSFTypes,OSFAdvanced}.pex + Source/*.psc
│  └─ OSF/internal.osf.json            (system scenes: health self-test solo + pair smoke-test)
├─ Library/OSF/vanilla/*.osf.json      (opt-in: vanilla + creature animation library)
└─ Immersion/OSF/immersion/emotes.osf.json   (opt-in: emote-wheel content)
```

Settings and hotkeys live in OSF UI's in-game settings menu (schema registered over the bridge at runtime) — no `Data/OSF/settings.json` ships.

## What OSF Animation does not package

- `OSFTest.pex/.psc` — the console smoke-test harness (dev-only; enforced by the script).
- `ui/animation-browser/` sources and `node_modules` — only the built output under `views/` ships.
- The `.pdb` by default (opt in with `-IncludePdb`).
- Anything under `tools/`, `test/`, `packaging/branding` (Nexus page assets — see `packaging/README` material and `nexus-page.bbcode`).
