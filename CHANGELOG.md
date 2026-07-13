# Changelog

All notable changes to OSF Animation are documented here.

## [Unreleased]

### Added
- Global hotkeys: an optional `"hotkeys"` map in `Data/OSF/settings.json` binds keys to `openBrowser` (open the scene browser), `openWheel` (emote wheel, below), and `toggleSceneTags:<tag[,tag..]>` (start a tag-matched scene via matchmaking / end it on re-press).
- Emote wheel: the `openWheel` hotkey opens the browser view in a radial mode listing solo scenes tagged under a prefix (default `player.emote.*`, so the immersion pack's emotes appear out of the box; `openWheel:<prefix>` overrides). Picking plays the emote on the player — or on the crosshair NPC captured at open time (dead / in-combat / non-human targets fall back to a player-only wheel) — then closes. Cancel with Esc, right-click, or the center hub. Adds the generic `osf.requestClose` bridge command (the view asks the host to hide it).
- Immersion content pack (`Data/OSF/immersion/`, pure JSON over vanilla `.af` clips): sit/lean-anywhere scenes on the `player.sit` / `player.lean` tags (full enter/idle/exit furniture chains for the leans; Space or hotkey re-press to stand) and self-terminating `player.emote.*` scenes (wave, what's up, hands on hips, arms crossed, data slate) for the emote wheel. Documents the well-known tag contract in `docs/SCENE_SCHEMA.md`.

### Changed
- Vanilla library packs now exclude **partial-coverage layer clips** (real tracked bones < 85% of the rig, measured from the `.af` index atlas — e.g. cover-lean or `*_idlepartialbody_*` layers with 5-25 of 82 bones): played standalone they T-pose every untracked bone. ~1,700 of ~14k clips dropped from the browsable catalog; regenerate with `generate_vanilla_packs.py --include-partial` to re-add them tagged `"partial"`.

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
