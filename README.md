# OSF Animation

OSF Animation is a native animation and scene-runtime framework for Starfield.

Key Features:
- Synced multi-actor AF/GLTF/GLB playback
- Optional OSF Animation Browser addon with its own native DLL, the scene browser, animation library (4,600+ base-game clips incl. creatures), and emote wheel
- Engine-native Wwise loose-file audio playback (rides the game mix; no private-device fallback)
- Actor anchoring/pinning in the world
- A shared scene registry loaded from `Data/OSF/**/*.osf.json`
- Player-control and camera locks
- Optional full-screen fade effects
- Equipment hide/restore and scene-scoped equip
- Scene graphs with nodes, cues, actions, callbacks, and navigation

## Documentation

- **Scene and animation authors:** [OSF Studio](https://github.com/ozooma10/osf-web-services/tree/main/sites/studio)
  provides the local-first browser authoring workspace.

- **Quest/consumer-mod authors:** [docs/API.md](docs/API.md) - Papyrus integration guide and the API stability policy;
  per-native reference in [dist/Scripts/Source/OSF.psc](dist/Scripts/Source/OSF.psc).

- **Pack authors:** [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)
  ship animation scenes with JSON plus GLB/AF clips, no Papyrus or scripting required; full schema in [docs/SCENE_SCHEMA.md](docs/SCENE_SCHEMA.md).

- **Developers:** [profiling workflow](docs/PROFILING.md) covers WPR/WPA CPU sampling.
- **Before releasing:** run `pwsh -NoProfile -File tools/test-release.ps1` for native,
  browser, installer and disposable in-game checks. Use `-Offline` to omit the game
  or `-Plan` to inspect the suite. [Testing guide](docs/SMOKE_TESTING.md) explains
  prerequisites, retained reports and the remaining acceptance boundaries.

## Requirements

- **SFSE** matching your game version.
- **Address Library for SFSE Plugins**
- **OSF Settings 1.0.0+** — owns engine settings, diagnostics, and hotkeys.
- **OSF UI 2.0.0+ and OSF Animation Browser** — optional, together provide the in-game browser and wheel. Open **OSF Animation Browser** in Settings' Launcher tab or bind an Animation hotkey.
  Playback and the native/Papyrus APIs remain available with both Browser and OSF UI absent.
  See [native addon ownership and public services](docs/BROWSER_ADDON.md).

## Installing or upgrading to 1.5.0

Install **OSF Animation v1.5.0.zip** for the engine. For the browser and wheel, also
install **OSF Animation Browser v1.5.0.zip** and OSF UI 2.0.0+. The Browser installer
offers the optional vanilla/creature library and emote content. OSF Settings 1.0.0+
provides settings and the launcher.

Replace the old Animation mod files instead of merging; preserve custom content
and saved user data. Install matching engine/Browser versions and **rebind the two
browser hotkeys**. Preferences from OSF Settings import once; values from the older
combined OSF UI service do not. Wheel pins and scene IDs remain unchanged.

Follow the [1.5.0 upgrade guide](docs/UPGRADING.md) for the replacement steps,
settings migration details, and first-launch checks. See the [changelog](CHANGELOG.md)
for the complete release notes.

### Build (to compile from source)

- XMake 3.0.0+
- C++23 compiler, usually MSVC

## Build

```bat
xmake f -m releasedbg
xmake
```

`xmake` builds both native DLL targets. `xmake build "OSF Animation"` builds only the engine.
The engine source lives in `src/`; the browser extension owns `src/`, `ui/`,
`packaging/fomod/`, and its native target under `extensions/browser/`.
The root build explicitly includes `extensions/browser/xmake.lua`.
When `XSE_SF_MODS_PATH` is set, xmake installs each plugin and its distribution files
under:

```text
%XSE_SF_MODS_PATH%\OSF Animation\
%XSE_SF_MODS_PATH%\OSF Animation Browser\
```

The optional browser has its own build and MO2 folder:

```powershell
pwsh -NoProfile -File tools/BuildBrowserAddon.ps1
```

That command builds the addon DLL, verifies the web view, and deploys both to `%XSE_SF_MODS_PATH%\OSF Animation Browser\` when the environment variable is set. Release archives are built separately with `packaging/build-archive.ps1` and `packaging/build-browser-addon.ps1`.

The Papyrus API is exposed as `OSF.*`.

### Intellisense
```bat
xmake project -k compile_commands   
```

## Tests

```bat
xmake test
```

runs the engine-free C++ suites (persistence broker, native scene-event callbacks, frame clock/clip
specs, additive pose math, sound registry, scene registry, and generated
GLTF/GLB/gzip import fixtures, plus the browser/engine API boundary). The browser view has its own suite:
`npm --prefix extensions/browser/ui run verify`.

Run the smoke bench to build production binaries, execute the native and browser suites,
validate both release archives, and retain logs plus JSON/Markdown/JUnit evidence:

```powershell
pwsh -NoProfile -File tools/test-smoke.ps1
pwsh -NoProfile -File tools/test-smoke.ps1 -RunGame
```

`-RunGame` also builds a separate test DLL and runs the sibling OSF Test Harness's
Animation scenario against the published Settings v1.0.0 and UI v2.0.0 archives in a
disposable profile. Native browser open/close/reopen is included; rendering and controller
acceptance remain separate. `-Plan` prints the inventory without building or
launching anything. Reports live in `build/smoke/<run>/report.md`. See
[docs/SMOKE_TESTING.md](docs/SMOKE_TESTING.md) for prerequisites and coverage boundaries.

## Test Harness

```bat
xmake build osf-import-test
osf-import-test.exe <file.glb>

xmake build osf-af-import-test
osf-af-import-test.exe <clip.af> <skeleton.rig>
```

These harnesses exercise the offline GLTF/GLB and AF import paths without launching the game.

## License & Credits

OSF Animation is licensed under the **GNU General Public License v3.0** ([LICENSE](LICENSE)),
with an additional modding/linking exception - see [EXCEPTIONS](EXCEPTIONS).

It adapts and builds on several open-source projects; full attributions and licenses are in
[THIRD_PARTY.md](THIRD_PARTY.md). Most notably:

- **[NativeAnimationFrameworkSF](https://github.com/Deweh/NativeAnimationFrameworkSF)** (Deweh, GPL-3.0) - the ozz playback/graph plumbing OSF is adapted from.
- **[CALUMI.Animation](https://github.com/Calaverah/CALUMI.Animation)** (Calaverah, LGPL-3.0) - the engine-native `.af` / `skeleton.rig` decoder.
- **[CommonLibSF](https://github.com/libxse/commonlibsf)**, fastgltf, ozz-animation, zlib, nlohmann/json, and miniaudio.
