# OSF Animation

OSF Animation is a fully native, Animation and Scene framework for Starfield.

Inspired by NAFSF, it provides native external-format (GLTF/GLB) animation playback alongside a comprehensive data-driven scene runtime.

Key Features:
- Synced multi-actor GLTF/GLB playback
- wWise Audio playback, with wav fallback support
- actor anchoring/pinning within world
- a shared registry of animations/scenes
- Player Control/Camera Lock
- Full Screen Fade effects
- Equipment removal/adding
- scene runtime providing graphs of nodes with cues, actions, callbacks and navigation

## Documentation

- **Quest/consumer-mod authors:** [docs/API.md](docs/API.md) - Papyrus integration guide and the API stability policy;
  per-native reference in [dist/Scripts/Source/OSF.psc](dist/Scripts/Source/OSF.psc).

- **Pack authors:** [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)
  ship advanced animation scenes with JSON + GLB, no papyrus or scripting required; full schema in [docs/SCENE_SCHEMA.md](docs/SCENE_SCHEMA.md).
  

## Requirements

- **SFSE** matching that game version.
- **Address Library for SFSE Plugins**

### Build (to compile from source)

- XMake 3.0.0+
- C++23 compiler, usually MSVC

## Build

```bat
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
```

The harness exercises the offline GLTF import path without launching the game.