# OSF Animation

OSF Animation is a native animation and scene-runtime framework for Starfield.

Key Features:
- Synced multi-actor AF/GLTF/GLB playback
- In-game scene browser, animation library (4,600+ base-game clips incl. creatures), and emote wheel (hosted by OSF UI)
- Engine-native Wwise loose-file audio playback (rides the game mix; no private-device fallback)
- Actor anchoring/pinning in the world
- A shared scene registry loaded from `Data/OSF/**/*.osf.json`
- Player-control and camera locks
- Optional full-screen fade effects
- Equipment hide/restore and scene-scoped equip
- Scene graphs with nodes, cues, actions, callbacks, and navigation

## Documentation

- **Quest/consumer-mod authors:** [docs/API.md](docs/API.md) - Papyrus integration guide and the API stability policy;
  per-native reference in [dist/Scripts/Source/OSF.psc](dist/Scripts/Source/OSF.psc).

- **Pack authors:** [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)
  ship animation scenes with JSON plus GLB/AF clips, no Papyrus or scripting required; full schema in [docs/SCENE_SCHEMA.md](docs/SCENE_SCHEMA.md).
  

## Requirements

- **SFSE** matching your game version.
- **Address Library for SFSE Plugins**
- **OSF UI** — hosts the scene browser, emote wheel, settings menu, and hotkeys. The engine
  runs without it, but the entire in-game UI is unavailable.

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

The Papyrus API is exposed as `OSF.*`.


### Intellisense
```bat
xmake project -k compile_commands   
```

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
