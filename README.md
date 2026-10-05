# OSF Animation

OSF Animation is a native animation and scene-runtime framework for Starfield.

Key Features:
- Synced multi-actor AF/GLTF/GLB playback
- Optional OSF Animation Browser addon with the scene browser, animation library (4,600+ base-game clips incl. creatures), and emote wheel
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

- **Architecture:** [actor animation composition and state routing](docs/RFC-actor-animation-runtime.md)
  proposes how equipment routes, persistent constraints, locomotion profiles, and cinematic scenes
  can share one per-actor pose runtime.

- **Developers:** [profiling workflow](docs/PROFILING.md) covers WPR/WPA CPU sampling.

## Requirements

- **SFSE** matching your game version.
- **Address Library for SFSE Plugins**
- **OSF Settings 1.0.0+** — owns engine settings, diagnostics, and hotkeys.
- **OSF UI 2.0.0+ and OSF Animation Browser** — optional, together provide the in-game browser and wheel. Open **OSF Animation** in Settings' Launcher tab or bind an Animation hotkey.
  Playback and the native/Papyrus APIs remain available without the browser.

Upgrading from the combined OSF UI settings service starts with the new Settings defaults;
old values and keybindings are not migrated. Both Animation hotkeys start unbound.

### Build (to compile from source)

- XMake 3.0.0+
- C++23 compiler, usually MSVC

## Build

```bat
xmake f -m releasedbg
xmake
```

When `XSE_SF_MODS_PATH` is set, xmake installs the plugin and distribution files
under:

```text
%XSE_SF_MODS_PATH%\OSF Animation\
```

The optional browser has its own build and MO2 folder:

```powershell
pwsh -NoProfile -File tools/BuildBrowserAddon.ps1
```

That command verifies the web view and deploys it to `%XSE_SF_MODS_PATH%\OSF Animation Browser\` when the environment variable is set. Release archives are built separately with `packaging/build-archive.ps1` and `packaging/build-browser-addon.ps1`.

The Papyrus API is exposed as `OSF.*`.

### OSF Studio live preview

Enable **Studio Link** under OSF Animation → Advanced → Authoring to create a session-scoped inbox
under `Documents\My Games\Starfield\SFSE\OSF\Studio Link`. OSF Studio's **Preview in game** button
can pair with that folder and play its current unsaved human `.af` on the player at normal speed.
The authoring service is off by default; disabling it stops its worker and restores any preview it
owns. Studio previews are transient, never enter the registry, and refuse to replace non-Studio playback.


### Intellisense
```bat
xmake project -k compile_commands   
```

## Tests

```bat
xmake test
```

runs the engine-free C++ suites (persistence broker, native scene-event callbacks, frame clock/clip
specs, additive pose math, sound registry, scene registry, overlay route contracts, and generated
GLTF/GLB/gzip import fixtures). The browser view has its own suite:
`npm --prefix ui/animation-browser run verify`.

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
