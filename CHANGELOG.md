# Changelog

All notable changes to OSF Animation are documented here.

## [Unreleased]

### Added
- Global hotkeys: an optional `"hotkeys"` map in `Data/OSF/settings.json` binds keys to `openBrowser` (open the scene browser), `openWheel` (emote wheel — stub until the wheel UI ships), and `toggleSceneTags:<tag[,tag..]>` (start a tag-matched scene via matchmaking / end it on re-press).
- Immersion content pack (`Data/OSF/immersion/`, pure JSON over vanilla `.af` clips): sit/lean-anywhere scenes on the `player.sit` / `player.lean` tags (hold pose, Space or hotkey re-press to stand) and self-terminating `player.emote.*` scenes (wave, what's up, hands on hips, arms crossed, coffee, data slate) for the emote wheel. Documents the well-known tag contract in `docs/SCENE_SCHEMA.md`.

## [0.2.0] - 2026-06-30

### Added
- SAF backwards-compatibility shim (opt-in FOMOD component): `SAF.pex` / `SAFScript.pex` that expose SAF API and route to OSF's natives, so SAF-targeting content runs on OSF. 

## [0.1.0] - 2026-06-27

First alpha.

### Added
- Native GLTF/GLB animation playback with synced multi-actor scenes and a shared frame clock.
- Starfield engine-native `.af` + `skeleton.rig` import path (ozz-backed, plays through the same graph as GLB content).
- Data-driven scene runtime: node graphs with cues, actions, callbacks, and navigation (`*.osf.json`), discovered from `Data/OSF/**`.
- player-control / camera lock, full-screen fade, equipment hide/restore, engine-native Wwise loose-file audio (no private-device fallback), subtitles, and HUD messages.
- Papyrus API (`OSF.*`) plus the `OSFTest` console smoke-test harness.
