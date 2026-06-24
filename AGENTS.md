# OSF Animation

SFSE plugin - the **engine** of **OSF**, a scene framework for Starfield (NAFSF playback lineage). It hosts native playback, sync, the shared clock, anchoring, a clip/pack registry, and the **scene runtime** (graphs of nodes with cues/actions/callbacks/navigation). The engine stays
**content-neutral**: it provides policy *mechanisms* (control/camera lock, fade, equipment-strip,
voice/sound, camera hold);

## Build

- **Native (C++/DLL):** build + deploy with `xmake` (mode `releasedbg` — `xmake f -m releasedbg`,
  then `xmake`). This is the only thing an agent compiles.
- **Papyrus (`dist/Scripts/Source/*.psc` → `*.pex`): do NOT compile.** When you edit a `.psc`,
  update the source (and any docs) and stop — leave the stale `.pex` as-is. The user recompiles the
  Papyrus scripts manually before in-game testing. Don't extract base scripts or invoke
  PapyrusCompiler; just note in your summary that the `.pex` needs a manual recompile.

## Architecture

Each entry: **system** (`path`) — role. RE detail lives in **docs/RE.md**.

### Layer A - core playback
- **GLTFImport** (`src/Serialization/GLTFImport.*`) — fastgltf 0.9 → ozz skeleton + runtime anim;
  handles NAF gzip GLBs; process-wide import cache cleared by `ReloadPacks()`. No CLSF dep.
- **Graph** (`src/Animation/Graph.*`) — per-actor sampler/stamper across two hooks: `Sample()`
  (advances time/stage, keeps rig binding warm) + `StampPose()` (samples ozz pose ONCE/frame, writes
  engine flat rig buffers, NiTransform ROW layout — do NOT transpose).
- **GraphManager** (`src/Animation/GraphManager.*`) — graph map keyed on `TESObjectREFR*`; 
  owns the two vtable hooks. `PlaySceneStaged` is the one scene entry. `StopAll` drops all state on load (releases the persistent AI-driven flag). 
  Compose-root pin + cull-sphere + BSFadeNode fade suppression live in `Hook_ModelNodeUpdate`
- **FrameClock** (`src/Animation/FrameClock.h`) — owner-token clock so subdivided updates advance 1×/ frame; defines `SyncGroup`.
- **Scene** (`src/Animation/Scene.*`) — pure Layer-A: shared clock + participant graphs + anchor +
  per-stage {files, placements, timer, loops, blend}. Auto-advances on timer/loop-target.
- **IdlePlayer** (`src/Animation/IdlePlayer.*`) — separate playback door: plays a modder-supplied
  loose `.af` on any actor through the engine's OWN graph (a template `TESIdleForm`'s GNAM repointed
  per play + native `PlayIdle`, archetype-swap for the player), NOT the ozz sampler. Pure-native
  (`RE::ActorUtils`, REL::ID; no Papyrus VM). Template pool ships in OSF.esm; archetype keywords are
  fixed base-Starfield forms (not settings). `OSF.PlayIdleFile`.
- **PackRegistry** (`src/Registry/PackRegistry.*`) — loads SLAL-shaped JSON animation packs from
  `Data/OSF/**` (mechanical schema only; content fields ignored — policy lives in `*.scene.json`).
- **Player/Camera locks** (`src/Player/PlayerControlService.*`, `src/Camera/CameraService.*`) —
  standalone locks (input-disable + AI-driven + third-person hold). 
  Applied by the runtime's `osf.control.lock` action (ref-counted per scene).
- **Papyrus** (`src/Papyrus/OSFScript.*`) — natives bound via GameVM at kPostDataLoad, re-bound every
  load (VM is rebuilt).
- **Save-safety** (`src/Serialization/SaveSafety.*`) — `GraphManager::StopAll` drops ALL scene/graph
  state on a world-replacing load (SaveLoadEvent begin sink + TESLoadGameEvent backstop + manual).
- **Startup** (`src/main.cpp`) — logs game-version vs RE build, loads packs+scenes, inits SoundService
  + applies `Settings`, registers the runtime with GraphManager, emits a feature report.

### Layer B - scene runtime
- **SceneRegistry** (`src/Registry/SceneRegistry.*`) - loads `*.scene.json` graphs (nodes + edges +
  roles + loop/timer + cue/action/sound/camera tracks) + validation (`GetSceneLoadErrors`).
- **SceneRuntime** (`src/Scene/SceneRuntime.*`) - generational handle table, lifecycle (`Fire`), navigation, timed-mark decode (`OnTimedMarks`, lane-ordered action→camera→sound→cue)
  `osf.*` action dispatch (`RunAction`), and the **per-handle undo ledger** (reverses every reversible
  mechanism in reverse order, once, idempotently, on ANY termination). Talks to Layer A only via its public surface, to Layer C via services.
- **SceneEventRelay** (`src/Scene/SceneEventRelay.*`) - token registry + async C++->Papyrus dispatch of
  the `OSFEvent:SceneEvent` struct.
- **Matchmaking** (`src/Matchmaking/Matchmaker.*`) — unified candidate pool over `SceneRegistry` defs
  + `PackRegistry` packs; deterministic role-binding; `roles[].filters` gender/keyword/race with
  `"Plugin.esm|0xID"` form-refs resolved at load (RE-sensitive — needs in-game verification).

### Layer-C - services 
- (`src/UI/FadeService.*`, `src/Equipment/EquipmentService.*`, `src/Audio/{SoundService,WwiseBackend}.*`, Player/Camera locks, `src/Config/Settings.*`) -
  content-neutral *mechanisms* with NO scene knowledge; each prologue-gates its engine calls and
  self-disables on mismatch. Whether a mechanism runs is driven by the scene/API alone — there is no
  user-settings feature toggle; `Data/OSF/settings.json` only tunes behaviour (e.g. `soundVolume`).