# Changelog

All notable changes to OSF Animation are documented here.

## [Unreleased]

### Added
- **Scene gear** — user-side scene equipment. Register any ARMO as gear in a slot (e.g. `belt`) via a `*.osfgear.json` anywhere under `Data/OSF/` (shippable by gear mods) or hand-edit `Documents\My Games\Starfield\SFSE\OSF\scene-gear.json`; a registered item **carried by a scene participant** is auto-equipped for the scene's duration (ledger-reversed on every end path, no inventory residue) — and if already worn, it's exempted from the default strip instead, so pre-equipping it "just works". At most one item per slot per actor: a per-actor `overrides` entry in `scene-gear.json` picks between multiples (or `"none"` suppresses a slot), else a worn item wins, else stable order. A scene's authored role `equip` beats user gear for the slot it occupies. Global toggle: settings → Scene gear → **Auto-equip scene gear** (default on). Cast-manager UI for picking/registering gear lands in a follow-up.
- Gamepad camera control for the orbit camera: the **right stick** orbits (like mouse drag), the **left stick** flies the orbit center (like WASD), and the **triggers** zoom (RT in / LT out, like the wheel). Works in scene orbits and the browse orbit alike — the DLL polls XInput directly, so it keeps working while the browser overlay owns game input.
- Gamepad UI navigation while the scene browser is open: the **D-pad** moves focus (hold to repeat), **A** activates, **B** cancels the wheel / closes the browser — and the sticks stay on the camera. The view takes OSF UI's `osfui.gamepadRaw` grant (protocol 1.0) so the runtime's default mapping no longer routes the sticks into UI nav/scroll, and re-creates the D-pad/A/B half itself from raw `ui.gamepad` events.
- Scene browser: scenes are now organized into **collapsible per-pack groups**, so a big install (e.g. a compat pack spanning dozens of scene files) folds to a few headers instead of one endless list. Grouping keys on a new optional file-level `pack` field in `*.osf.json` (the content-pack display name — set the same string in every file of a pack to merge them into one group; see `docs/SCENE_SCHEMA.md`), falling back to the scene file's name. Groups auto-expand while searching, while the list is short, or around the selected scene; pack names are searchable. Catalog/library bridge payloads gain `pack` and `sourceFile` fields (additive).
- The scene browser's status line now also shows the installed OSF UI host version. If it predates the release this build was tested against (1.1.0), an amber `UPDATE` badge appears linking the [OSF UI Nexus page](https://www.nexusmods.com/starfield/mods/17711) — clicking it in-game opens the page in your system browser (OSF UI's `osfui.openModPage` command: the URL is hardcoded in OSF UI, nothing crosses the bridge), and the SFSE log gains a matching warning.

### Changed
- The status line is now compact (`OSF 1.0.0 · UI 1.1.0`) so it can't push the header layout down; the full identity moved to its tooltip. The plugin version no longer renders a trailing unused build field (`1.0.0.0`).

### Fixed
- Scene-browser camera handling aboard ships. Opening the browser from the **pilot seat** no longer force-switches to the on-foot third-person camera (a stranded "staring at your own hull / void" view that nothing restored — the cockpit registers as first person to the engine, so the browser's can't-see-yourself kick fired on seated pilots too; it now skips them). Closing the browser (or any scene camera ending) now returns a pilot to the **cockpit or whichever ship view they had**: camera baselines are captured as the exact engine state and engine-owned states are handed back directly, because the on-foot first/third-person re-entries silently bail for a seated pilot. The post-load leaked-camera recovery restores the cockpit for pilots the same way.
- The browse orbit no longer engages while **aboard a ship in space** (dragging the world area simply does nothing there; landed ships orbit as before). The orbit drives the camera in absolute world coordinates captured once at engage, and space cells re-base/move under the ship interior — the result was the camera violently spinning around the hull.

## [1.0.0] - 2026-07-20

### Added
- Global hotkeys, configured in OSF UI's in-game settings menu (OSF Animation card → Hotkeys): open scene browser and open emote wheel. Rebindable in-game with conflict badges; the wheel defaults to `B` (free in vanilla, the emote-wheel convention key), the browser stays unbound (F10 — the OSF UI console toggle — already opens it). Delivered by OSF UI's HotkeyService (bridge ABI 1.4), so keys never fire while the console is open or a text field is focused.
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
