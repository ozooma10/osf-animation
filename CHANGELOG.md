# Changelog

All notable changes to OSF Animation are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project aims to follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

First alpha — in progress, not yet released. Stamp the version (`0.1.0`) and date here on launch.

### Added
- Native GLTF/GLB animation playback with synced multi-actor scenes and a shared frame clock.
- Starfield engine-native `.af` + `skeleton.rig` import path (ozz-backed, plays through the
  same graph as GLB content).
- Data-driven scene runtime: node graphs with cues, actions, callbacks, and navigation
  (`*.osf.json`), discovered from `Data/OSF/**`.
- Layer-C mechanisms: player-control / camera lock, full-screen fade, equipment hide/restore,
  Wwise audio (with loose-file WAV fallback), subtitles, and HUD messages.
- Papyrus API (`OSF.*`) plus the `OSFTest` console smoke-test harness.
