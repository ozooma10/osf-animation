# Changelog

All notable changes to OSF Animation are documented here.

## [Unreleased]

### Added
- Global hotkeys, configured in OSF UI's in-game settings menu (OSF Animation card → Hotkeys): open scene browser and open emote wheel. Rebindable in-game with conflict badges; unbound by default. Delivered by OSF UI's HotkeyService (bridge ABI 1.4), so keys never fire while the console is open or a text field is focused.
- Settings moved into OSF UI's settings menu (schema registered at runtime over the native bridge): log level, stage-transition debug popups, and first-run hint. Values persist in `Documents\My Games\Starfield\OSFUI\settings\osf.json`. **`Data/OSF/settings.json` is no longer read** (a one-time log warning fires if one is present); with OSF UI absent or too old, defaults apply and hotkeys are unavailable.
- Emote wheel: the `openWheel` hotkey opens the browser view in a radial mode listing solo scenes tagged under a prefix (default `player.emote.*`, so the immersion pack's emotes appear out of the box; `openWheel:<prefix>` overrides). Picking plays the emote on the player — or on the crosshair NPC captured at open time (dead / in-combat / non-human targets fall back to a player-only wheel) — then closes. Cancel with Esc, right-click, or the center hub. Adds the generic `osf.requestClose` bridge command (the view asks the host to hide it).
- Immersion content pack (`Data/OSF/immersion/`, pure JSON over vanilla `.af` clips): self-terminating `player.emote.*` scenes (wave, what's up, hands on hips, arms crossed, data slate) for the emote wheel. Documents the well-known tag contract in `docs/SCENE_SCHEMA.md`.
- Browse orbit: dragging the world area of the scene browser now always steers an orbit camera. With no scene camera live (no scene running, or a `camera:"none"` scene like an emote), the first drag engages an orbit around the player's scene cast (or the player); closing the browser restores the previous view. Previously the drag only worked while a `scene_orbit` scene was playing — with the browser open, OSF UI freezes all game input, so the camera was otherwise immovable.

- Badge icon in OSF UI's Mods surface: the rail entry and Home cards now show the OSF playback-curve mark (`views/osf/osf-icon.svg`, schema `icon` field — a tiny-size cut of the branding emblem, bolded to stay legible at ~30px) instead of the "OA" initials monogram. Needs an OSF UI new enough to know the field; older versions ignore it and keep the initials.

- Emote-wheel pinning: a `◇ PIN TO WHEEL` toggle in the scene browser's brief puts any solo authored scene on the emote wheel (pinned rows show a ◆ marker). With pins present the wheel shows exactly them, in pin order; with none it falls back to the `player.emote.*` tag pool as before. The list persists DLL-side in `Documents\My Games\Starfield\OSF\wheel-pins.json` (account-global; pins whose pack is uninstalled are kept dormant and revive on reinstall). New bridge command `osf.animation.wheel.pin {sceneId, pinned}`, answered by a catalog re-push with per-scene `pinned` order.

### Removed
- SAF backwards-compatibility shim (the opt-in `SAF.pex`/`SAFScript.pex` FOMOD component from 0.2.0) and its supporting internals: the non-public `OSFCompat` natives, the Activate-key redirect in the input hook, and the FOMOD "Compatibility" install step. SAF-targeting content should use the real SAF, or be ported to the OSF API.

### Changed
- Emote-wheel ring geometry is now count-adaptive: near-circular for a handful of emotes (the shipped pack's ~5 no longer stretch into a flat oval), widening toward the old 12-slice ellipse as the ring fills.
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
