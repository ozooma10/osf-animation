# OSF Animation

OSF Animation is an SFSE plugin — the native animation-playback core of the OSF scene
framework for Starfield (a SAF/NAFSF-lineage replacement): native GLTF/GLB playback,
synced multi-actor scenes, shared-clock frame-locking, anchoring/pinning, staged
advancement, a clip/pack registry, and a SAF compatibility shim. It is **content-neutral** —
scene *policy* (undress, scheduled voice, camera/control, fade choreography, scene/cue
callbacks) lives in the separate OSF Intimacy scene engine, not here. See
[DESIGN.md](DESIGN.md) §8 and [CLAUDE.md](CLAUDE.md) for architecture and dev notes.

## Documentation

- **Pack authors:** [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) — ship
  animations with JSON + GLB, no C++; full schema in
  [docs/PACK_SCHEMA.md](docs/PACK_SCHEMA.md).
- **Quest/consumer-mod authors:** [docs/API.md](docs/API.md) — Papyrus integration
  guide and the API stability policy; per-native reference in
  [dist/Scripts/Source/OSF.psc](dist/Scripts/Source/OSF.psc).

## Requirements

### Runtime (to play with the mod)

- **Starfield 1.16.244.0** — last verified build.
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

## Log viewing

The plugin logs to `Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log`. Lines are
`[time] [thread] [level] message` with spdlog level letters (T/D/I/W/E/C), e.g.
`[16:39:17] [38092] [W] Sync: actor 14 has no live graph — skipping`.

[klogg](https://klogg.filimonov.dev/) can color them by severity: under **Tools → Highlighters
Configuration**, add one rule per level, anchored to the level field so message text can't
false-match. They apply live in Follow mode (`F`):

- `^\[[^\]]*\] \[\d+\] \[[EC]\]` — error/critical
- `^\[[^\]]*\] \[\d+\] \[W\]` — warning
- `^\[[^\]]*\] \[\d+\] \[[TD]\]` — trace/debug
