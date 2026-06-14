# OSF Animation

SFSE plugin — the native animation-playback **core** of **OSF**, a SexLab-style scene framework
for Starfield (NAFSF/SAF lineage). This is the lean, **content-neutral** core (a SAF replacement):
playback, sync, shared clock, anchoring, the clip/pack registry, and the engine hooks. Scene
**policy** — undress/redress, scheduled voice, camera/control, fade choreography, scene/cue
callbacks, the stall watchdog, cosave aftermath — is **carved out** to the separate **OSF Intimacy**
scene engine (DESIGN.md §8). This file is the always-loaded session brief. Deeper docs:
architecture & rationale → **DESIGN.md** · launch roadmap → **LAUNCH.md** · RE/address ground
truth → **docs/RE.md** · the OSF Intimacy scene-engine boundary → **docs/INTIMACY_SEAM.md**.

> **This repo is the result of the "core carve"** (clean-cut, fresh history): the RE-verified
> native core was **migrated** from the pre-split `OSF Animation` repo, never rewritten. The
> archived pre-split repo (`OSF Animation Archive`) is the source the OSF Intimacy harvest draws
> from. **Consumer docs under `docs/` (API.md, PACK_SCHEMA.md, GETTING_STARTED.md, guide/) and
> TESTSUITE.md still describe the pre-carve full framework — they need a curation pass to match the
> lean native surface below.**

- **Build:** C++23, **xmake only** (no CMake/vcpkg). GPL-3.0 (NAFSF-derived; attribution in
  `src/Animation/Graph.h` + `GraphManager.h`). Based on
  [libxse/commonlibsf-template](https://github.com/libxse/commonlibsf-template). CLSF submodule on
  branch `forge`.
- **Version:** RE-verified against **1.16.244.0** (SFSE AddrLib v21 + self-made
  versionlib-1-16-244-0). Every engine binding gates before use → **after any game patch, work
  `docs/POST_PATCH_CHECKLIST.md`**. Address detail: `docs/RE.md`.
- **Target name is currently `OSF Animation Core`** (temporary, set in `xmake.lua` so this build
  does not clobber the live `OSF Animation` MO2 deploy during the carve). Flip back to
  `OSF Animation` at the folder swap (repo == xmake target == MO2 mod).

## Status snapshot

- **[LIVE] (migrated from the pre-split core, in-game verified there):** solo playback · 2-actor
  synced scenes · staged scenes w/ timer + loop-count auto-advance · blending · per-graph anchor
  (`SetAnchor`/`ClearAnchor`, rootMode pin/additive/follow) · retroactive `Sync` clock-merge ·
  `PlaySequence` solo multi-phase · `GetCurrentAnimation`/`SetSpeed`/`GetSpeed` · quickload teardown
  (graph-state save-drop) · co-load compat warning · version gating · readiness handshake.
- **[LIVE] standalone player locks (compat):** `OSFCompat.SetPlayerControlLock` /
  `SetPlayerCameraLock` — input-disable layer + AI-driven + third-person hold, for the SAF shim's
  primitive (non-Scene) Play+Sync path. Content-neutral mechanism; the core never auto-applies it.
- **[LIVE] SAF shim:** `SAF.psc` / `SAFScript.psc` forward to the lean surface
  (`Ping→IsReady`, `PlaySceneSeparate→StartSceneFiles`, `StopAnimation→Stop/StopScene`,
  `SyncGraphs→Sync`). Existing SAF content runs unchanged. The launch headline.
- **Carved out → OSF Intimacy (NOT in this repo):** `ScenePolicy`, undress/redress
  (EquipmentService), scheduled voice + SoundService/WwiseBackend, FadeService, EventRelay
  scene/cue callbacks, stall watchdog, Cosave aftermath persistence, the scene-integrated
  camera/control auto-apply.

## Native surface (lean, content-neutral)

Bound on `OSF` (see `dist/Scripts/Source/OSF.psc`):
- **Primitives:** `Play` · `Stop` · `SetSpeed`/`GetSpeed` · `SetAnchor`/`ClearAnchor` · `Sync` ·
  `PlaySequence` · `GetCurrentAnimation` · `IsPlaying`.
- **Scenes (mechanical: anchored, staged, synced — NO policy):** `StartScene` (registry id) ·
  `StartSceneByTags` (matchmake by tags + gender slots) · `StartSceneFiles` (ad-hoc raw files, the
  SAF `PlaySceneSeparate` replacement) · `SetSceneStage` · `GetSceneStage` · `StopScene` ·
  `FindScenes` · `ReloadPacks`.
- **Readiness:** `IsReady` · `HasFeature("scenes"/"playback"/"sync"/"anchor")` · `GetVersion`.
- **Misc:** `SetSceneControlMask` (debug) · `NotifyGameLoaded` (save-safety).
- **Compat (`OSFCompat`):** `SetPlayerControlLock` · `SetPlayerCameraLock`.

Natives are never removed / signatures stable within a major version.

## Build & dev loop

- **`xmake`** — builds + auto-installs `OSF Animation Core.dll` + `dist/Scripts/*.pex` +
  `dist/OSF/*.json` + `dist/OSF/Animations` to `MO2\mods\<target name>\` (via `XSE_SF_MODS_PATH`).
  First configure needs `-y`.
- **Papyrus recompile** (pass the script *name*, not a path), THEN re-run `xmake`:
  `& "C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe" OSF.psc -import="<repo>\dist\Scripts\Source;C:\Modding\Starfield\PapyrusSource" -output="<repo>\dist\Scripts" -flags="Starfield_Papyrus_Flags.flg" -optimize`
  (scripts: `OSF`, `OSFCompat`, `OSFTest`, `SAF`, `SAFScript`).
- **Test:** enable the mod in MO2 → launch via SFSE → read
  `Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log`. The `solo`/`pair`/`test.stages`/
  `test.loops` baked-in entries need no animation mod.
- **Offline import harness:** `xmake build osf-import-test` + `osf-import-test.exe <file.glb>`
  (builds without CLSF).

## Gotchas (footguns)

- **None-array native (CLSF):** a Papyrus `None` array asserts in `UnpackVariable`. Never declare
  `= None` array defaults.
- **Submodule rule:** **NEVER pin CLSF to an unpushed commit.** Push the fork branch FIRST, then
  bump the pointer. Prefer RE-derived layouts in THIS repo (with provenance comments) over
  fork-only struct edits.
- **CLSF `forge` (`5df499f`):** on any CLSF bump, verify **`GameVM::impl == 0x00E0`** → else
  BindNativeMethod assert at BSScriptUtil.h:388.
- **Dev-loop order:** edit a `.psc` → recompile → THEN `xmake` (after_build copies the pex).
- **Stale PCH after a repo move:** `build/.gens` bakes the absolute path; delete `build/`.
- **Nested `commonlib-shared` desync (after a CLSF bump):** `REX/W32/D3D12.h: No such file` →
  `git -C lib/commonlibsf submodule update --init --recursive lib/commonlib-shared`.

## Architecture

Each entry: **system** (`path`) — what it does. Addresses/offsets/RE detail live in **docs/RE.md**.

### Core pipeline
- **GLTF import** (`src/Serialization/GLTFImport.*`) — fastgltf 0.9 → ozz skeleton + runtime
  animation (bones only). Handles NAF gzip GLBs. Process-wide import cache, cleared by
  `OSF.ReloadPacks()`. No CLSF dep.
- **Graph** (`src/Animation/Graph.*`) — per-actor sampler/stamper, split across two mandatory
  hooks: `Sample()` (advances time/stage + keeps the rig binding warm) and `StampPose()` (samples
  the ozz pose ONCE per frame, writes engine flat rig buffers, NiTransform ROW layout — do NOT
  transpose). Migrated verbatim from the pre-split core.
- **GraphManager** (`src/Animation/GraphManager.*`) — graph map keyed on `TESObjectREFR*` + scene
  list; owns the two vtable hooks (verify slot/bytes before patching). `PlaySceneStaged` is the one
  scene entry (validate → preload all stages → build Scene → teleport-if-anchored → publish →
  animation-driven switch for anchored scenes). `StopAll` drops all state on a load (also releases
  the persistent AI-driven flag — save-safety). The compose-root pin + cull-sphere + BSFadeNode
  near-camera fade suppression live in `Hook_ModelNodeUpdate` (migrated verbatim — RE-proven).
- **FrameClock** (`src/Animation/FrameClock.h`) — owner-token clock so subdivided update calls
  advance time exactly 1×/frame. Also defines `SyncGroup`.
- **Scene** (`src/Animation/Scene.*`) — content-neutral: shared clock + participant graphs + world
  anchor + per-participant placements + per-stage {files, placements, timer, loops, blend}.
  `Advance` auto-advances on timer/loop-target; `loopWhole` (PlaySequence) restarts at stage 0;
  after the last stage sets `ended` (hook defers StopScene to the game thread). **No voice/cues/
  equipment/policy/events/watchdog** — those left for OSF Intimacy.
- **PackRegistry** (`src/Registry/PackRegistry.*`) — loads SLAL-shaped JSON from `Data/OSF/**`.
  Parses the mechanical schema (tags, gender slots, stages, clips, offsets, timer/loops); the OSF
  content fields (undress/equipment/voice/intensity/peak/cues) are **ignored**. `*.voice.json` /
  `*.dialogue.json` skipped (OSF Intimacy content). Cross-pack id collisions: case-insensitive,
  first-load-wins. Schema doc: **docs/PACK_SCHEMA.md** (stale — content fields removed from the core).
- **Player/Camera locks** (`src/Player/PlayerControlService.*`, `src/Camera/CameraService.*`) —
  standalone-only locks for the SAF shim (input-disable layer + AI-driven + third-person hold/POV
  bounce). No scene integration (that is OSF Intimacy).
- **Co-load warning** (`src/UI/CompatWarning.*`) — probes at `kPostLoad` for SAF/NAFSF (rig-stamping
  conflict) via `GetModuleHandle`; logs + a blocking Win32 `MessageBoxA`.
- **Papyrus** (`src/Papyrus/OSFScript.*`) — natives bound via GameVM at kPostDataLoad + re-bound on
  every load (the VM is rebuilt). `OSF.psc` + pex ship.
- **Save-safety** (`src/Serialization/SaveSafety.*`) — `GraphManager::StopAll` drops ALL scene/graph
  state on a world-replacing load (SaveLoadEvent begin sink + TESLoadGameEvent backstop + manual
  `OSF.NotifyGameLoaded()`); the backstop also re-binds the natives onto the rebuilt VM. **No cosave
  aftermath persistence** (carved to OSF Intimacy).
- **Startup** (`src/main.cpp`) — logs game-version vs RE build, emits a one-line feature report.

## Conventions

- License **GPL-3.0**. C++23, **xmake only**. Natives never removed / signatures stable within a
  major. Distribution: **LoversLab**; coordinate with NAFSF (Deweh) + SAF (mielu91m).
