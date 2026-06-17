# OSF Animation

SFSE plugin — the **engine** of **OSF**, a SexLab-style scene framework for Starfield (NAFSF/SAF
lineage, a SAF replacement). It hosts native playback, sync, the shared clock, anchoring, the
clip/pack registry, the engine hooks, the SAF shim, **and the scene runtime** — graphs of nodes
with cues, actions, callbacks, and navigation. (The scene engine — formerly the planned separate
"OSF Intimacy" plugin — was **merged in as an internal subsystem**; the three-plugin split was
reversed, see `docs/SCENE_DESIGN.md` §2.1.) It stays **content-neutral**: the engine provides the
policy *mechanisms* (player control/camera lock, fade, equipment-strip, scheduled voice/sound,
third-person camera hold — named neutrally; **all live as of Phase C, 2026-06-17**), while specific
adult content + orchestration live in the **OSF Seduce** content mod. This file is the always-loaded
session brief. Deeper docs:
architecture & rationale → **docs/SCENE_DESIGN.md** · launch roadmap → **LAUNCH.md** · RE/address
ground truth → **docs/RE.md** · the Layer A↔B scene-runtime seam → **docs/INTIMACY_SEAM.md**.

> **History:** the RE-verified playback core was **migrated** (never rewritten) from the pre-split
> `OSF Animation` repo; the **scene engine + content-neutral policy were then merged back in** as an
> internal subsystem (the earlier "OSF Intimacy" split reversed — `docs/SCENE_DESIGN.md` §2.1). The
> archived pre-split repo (`OSF Animation Archive`) was the harvest source for the Layer-C policy
> services (equipment/fade/voice/sound/camera) — **all landed via Phase C (complete 2026-06-17)**.
> Consumer docs reconciled from the pre-merge "policy lives elsewhere" framing to the merged reality
> (sweep 2026-06-17; flag/fix any residual stale lines as found).

- **Build:** C++23, **xmake only** (no CMake/vcpkg). GPL-3.0 (NAFSF-derived; attribution in
  `src/Animation/Graph.h` + `GraphManager.h`). Based on
  [libxse/commonlibsf-template](https://github.com/libxse/commonlibsf-template). CLSF submodule on
  branch `forge`.
- **Version:** RE-verified against **1.16.244.0** (SFSE AddrLib v21 + self-made
  versionlib-1-16-244-0). Every engine binding gates before use → **after any game patch, work
  `docs/POST_PATCH_CHECKLIST.md`**. Address detail: `docs/RE.md`.
- **Target name is `OSF Animation`** (set in `xmake.lua`) — the final shipping name; repo == xmake
  target == MO2 mod, so a build deploys to `MO2\mods\OSF Animation\`.

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
  `SyncGraphs→Sync`). Existing SAF **playback/sync/scene** content runs unchanged — the launch
  headline. Advanced SAF entry points with no core equivalent (phase/sequence-end callbacks,
  the crosshair selection buffer, blend-graph variables, absolute `SetActorPosition`) are no-op
  SHIM-GAP stubs. (The crosshair *target* — `GetCrosshairRef`/`GetCrosshairActor` + the pickers —
  is native via `OSFCompat` reading `PlayerCharacter->commandTarget`.)
- **[LIVE] scene runtime (merged in, Layer B):** the `*.scene.json` graph registry + generational
  int-handle table, lifecycle/navigation (`StartScene*`→handle, `AdvanceScene`/`NavigateScene`,
  stage interface, roles, exclusivity, load-safe handles), scene-event callbacks (`EventRelay`,
  async struct payload), and **all four track lanes** — `cue` (lifecycle + numeric + trigger-edge
  auto-take), `action`, `sound`, `camera` — driven by one generalized clip-timed mark + the §1.3
  same-tick order (action→camera→sound→cue).
- **[LIVE] Layer C policy mechanisms (Phase C, complete 2026-06-17; harvested from `OSF Animation
  Archive`):** built-in `osf.*` actions — `control.lock`/`release`, `fade.out`/`in`,
  `equipment.hide`/`restore`, `voice.play` — plus the `sound`/`camera` lanes; backed by content-neutral
  services (Player/Camera locks, FadeService, EquipmentService, SoundService/WwiseBackend). A
  **generalized per-handle undo ledger** reverses every reversible mechanism (control/fade/equipment/
  camera) in reverse order, once, idempotently, on ANY termination — cleanup never needs an authored
  release. User-settings precedence (silent-skip when disabled, `Data/OSF/settings.json`) + per-action
  required-field validation. **Deferred past v1** (additive, no API impact): free-fly/orbit camera
  states (runtime-OPEN in OSF RE), pool/set→clip metadata resolution, positioned Wwise posting.
  **Still external:** the **OSF Seduce** content layer (specific adult choreography/content/profiles);
  cosave aftermath persistence + the stall watchdog remain deferred.

## Native surface (lean, content-neutral)

Bound on `OSF` (see `dist/Scripts/Source/OSF.psc`):
- **Primitives:** `Play` · `Stop` · `SetSpeed`/`GetSpeed` · `SetAnchor`/`ClearAnchor` · `Sync` ·
  `PlaySequence` · `GetCurrentAnimation` · `IsPlaying`.
- **Scenes (anchored, staged, synced):** `StartScene` (registry id) · `StartSceneRoles` ·
  `StartSceneByTags` (matchmake by tags + gender slots) · `StartSceneFiles` (ad-hoc raw files, the
  SAF `PlaySceneSeparate` replacement) · `AdvanceScene`/`NavigateScene` · `GetSceneEdge*` ·
  `SetSceneStage` · `GetSceneStage` · `StopScene`/`StopSceneForActor` · `GetSceneId`/`GetSceneNode`/
  `GetSceneForActor` · `FindScenes` · `ReloadPacks` · `RegisterSceneCallback` + the `EVENT_*`/`RESULT_*`
  getters.
- **Readiness:** `IsReady` · `HasFeature("scenes"/"playback"/"sync"/"anchor")` · `GetVersion`.
- **Compat (`OSFCompat`):** `SetPlayerControlLock` · `SetPlayerCameraLock` · `SetSceneControlMask` (debug, off the public surface).

**Policy is data-driven, not natives:** the built-in `osf.*` actions + the cue/action/sound/camera
track lanes are authored in `*.scene.json` track entries (§1.3) and run by the scene runtime — they add
no Papyrus natives. Natives are never removed / signatures stable within a major version (the one
allowed pre-1.0 break — `StartScene*` bool/string→int handle — already landed).

## Build & dev loop

- **`xmake`** — builds + auto-installs `OSF Animation.dll` + `dist/Scripts/*.pex` +
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
  after the last stage sets `ended` (hook defers StopScene to the game thread). The `Scene` class is
  **pure Layer-A playback** — cues/actions/events/policy live in the Layer-B scene runtime
  (`src/Scene/SceneRuntime.*`), not here.
- **PackRegistry** (`src/Registry/PackRegistry.*`) — loads SLAL-shaped JSON **animation packs** from
  `Data/OSF/**`. Parses the mechanical schema (tags, gender slots, stages, clips, offsets,
  timer/loops); pack-level content fields (undress/equipment/voice/intensity/peak/cues) are
  **ignored** — scene policy lives in `*.scene.json` scene files (`SceneRegistry`), not packs.
  `*.scene.json` is handled by `SceneRegistry`; `*.voice.json` / `*.dialogue.json` reserved/skipped.
  Cross-pack id collisions: case-insensitive, first-load-wins. Schema doc: **docs/PACK_SCHEMA.md**.
- **Player/Camera locks** (`src/Player/PlayerControlService.*`, `src/Camera/CameraService.*`) —
  standalone locks (input-disable layer + AI-driven + third-person hold/POV bounce). Used by the SAF
  shim's primitive path AND by the scene runtime's `osf.control.lock` action (ref-counted per scene).
- **Co-load warning** (`src/UI/CompatWarning.*`) — probes at `kPostLoad` for SAF/NAFSF (rig-stamping
  conflict) via `GetModuleHandle`; logs + a blocking Win32 `MessageBoxA`.
- **Papyrus** (`src/Papyrus/OSFScript.*`) — natives bound via GameVM at kPostDataLoad + re-bound on
  every load (the VM is rebuilt). `OSF.psc` + pex ship.
- **Save-safety** (`src/Serialization/SaveSafety.*`) — `GraphManager::StopAll` drops ALL scene/graph
  state on a world-replacing load (SaveLoadEvent begin sink + TESLoadGameEvent backstop + manual
- **Startup** (`src/main.cpp`) — logs game-version vs RE build, loads packs+scenes, inits
  SoundService + applies `Settings`, registers the runtime with GraphManager, emits a feature report.

### Scene runtime (Layer B) + policy mechanisms (Layer C)
- **SceneRegistry** (`src/Registry/SceneRegistry.*`) — loads `*.scene.json` graphs (nodes + edges +
  roles + loop/timer + the cue/action/sound/camera track entries); validation + load diagnostics
  (`GetSceneLoadErrors`).
- **SceneRuntime** (`src/Scene/SceneRuntime.*`) — Layer B: the generational handle table, lifecycle
  (`Fire`), navigation, the timed-mark decode (`OnTimedMarks`, lane-ordered), the built-in `osf.*`
  action dispatch (`RunAction`), and the **per-handle undo ledger** (`RecordMechanism`/`UndoMechanism`/
  `ReplayLedger`). Talks to Layer A only via its public surface and to Layer C via the services.
  Registered with GraphManager through inverted callbacks (Layer A never names SceneRuntime).
- **SceneEventRelay** (`src/Scene/SceneEventRelay.*`) — token registry + async C++→Papyrus dispatch of
  the `OSFEvent:SceneEvent` struct (the callback transport).
- **Layer-C services** (`src/UI/FadeService.*`, `src/Equipment/EquipmentService.*`,
  `src/Audio/{SoundService,WwiseBackend}.*`, `src/Player/PlayerControlService.*`,
  `src/Camera/CameraService.*`, `src/Config/Settings.*`) — content-neutral *mechanisms* with NO scene
  knowledge; each prologue-gates its engine calls (verify-before-call) and self-disables on a mismatch.
  Drive these from Layer B via CLSF / stable game-facing APIs, never from Layer A's hooks (§2.2).

## Conventions

- License **GPL-3.0**. C++23, **xmake only**. Natives never removed / signatures stable within a
  major. Distribution: **LoversLab**; coordinate with NAFSF (Deweh) + SAF (mielu91m).
