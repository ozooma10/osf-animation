# OSF Animation

SFSE plugin - the **engine** of **OSF**, a scene framework for Starfield (NAFSF playback lineage). It hosts native playback, sync, the shared clock, anchoring, a clip/pack registry, and the **scene runtime** (graphs of nodes with cues/actions/callbacks/navigation). The engine stays
**content-neutral**: it provides policy *mechanisms* (control/camera lock, fade, equipment-strip,
voice/sound, camera hold);

## Build

- **Native (C++/DLL):** build + deploy with `xmake` (mode `releasedbg` — `xmake f -m releasedbg`,
  then `xmake`). This is the only thing an agent compiles.
- **Papyrus (`dist/Scripts/Source/*.psc` → `*.pex`): recompile after editing a `.psc`, THEN `xmake`
  to deploy the fresh `.pex`.** A stale `.pex` (source edited but not recompiled) makes any new/changed
  native fail to bind at link time — e.g. `error: Native static function X could find no matching
  static function on linked type OSF. Function will not be bound.` Compile with the CK Papyrus
  compiler (`OSFTypes.psc` sits in the import path so `OSFTypes:*` signatures resolve):
  ```powershell
  & "C:\Program Files (x86)\Steam\steamapps\common\Starfield\Tools\Papyrus Compiler\PapyrusCompiler.exe" `
    "dist\Scripts\Source" `
    -i="dist\Scripts\Source;C:\Modding\Starfield\PapyrusSource" `
    -o="dist\Scripts" -f="C:\Modding\Starfield\PapyrusSource\Starfield_Papyrus_Flags.flg" -all
  ```
  Output lands in `dist/Scripts/*.pex`; `xmake` copies it into `MO2/mods/OSF Animation/Scripts/`.
  Never edit base/vanilla Papyrus sources under `PapyrusSource/` — it is a read-only import path.

## Architecture

Each entry: **system** (`path`) — role.

### Layer A - core playback
- **GLTFImport** (`src/Serialization/GLTFImport.*`) — fastgltf 0.9 → ozz skeleton + runtime anim;
  handles NAF gzip GLBs; process-wide import cache cleared by `ReloadPacks()`. No CLSF dep.
- **AFImport** (`src/Serialization/AFImport.*`) — Starfield engine-native `.af` + `skeleton.rig` → ozz
  skeleton + runtime anim (same output shape as GLTFImport, so it plays through Graph). Decodes the
  quantized rest-relative tracks (ported from CALUMI) and re-bases to absolute local (`local = bind ∘
  track`); lets the ozz path source `.af`/vanilla content with full clock/sync. Rig bytes come via a
  caller-supplied provider (GraphManager: loose file, else read from the game BA2 via `Util::Ba2`),
  fetched once per session. No CLSF dep; offline test: `osf-af-import-test <clip.af> <skeleton.rig|@ba2>`.
- **Ba2** (`src/Util/Ba2.*`) — minimal Starfield BA2 "GNRL" reader (header + records + name table,
  zlib); pulls one file (the human `skeleton.rig`) out of the base archives so OSF ships no vanilla
  asset. Ported from glb2af `ba2extract.py`; texture (DX10) archives skipped.
- **Graph** (`src/Animation/Graph.*`) — per-actor sampler/stamper across two hooks: `Sample()`
  (advances time/stage, keeps rig binding warm) + `StampPose()` (samples ozz pose ONCE/frame, writes
  engine flat rig buffers, NiTransform ROW layout — do NOT transpose).
- **GraphManager** (`src/Animation/GraphManager.*`) — graph map keyed on `TESObjectREFR*`; 
  owns the two vtable hooks. `PlaySceneStaged` is the one scene entry. `StopAll` drops all state on load (releases the persistent AI-driven flag). 
  Compose-root pin + cull-sphere + BSFadeNode fade suppression live in `Hook_ModelNodeUpdate`
- **FrameClock** (`src/Animation/FrameClock.h`) — owner-token clock so subdivided updates advance 1×/ frame; defines `SyncGroup`.
- **Scene** (`src/Animation/Scene.*`) — pure Layer-A: shared clock + participant graphs + anchor +
  per-stage {files, placements, timer, loops, blend}. Auto-advances on timer/loop-target.
- **Player/Camera locks** (`src/Player/PlayerControlService.*`, `src/Camera/CameraService.*`) —
  standalone locks (input-disable + AI-driven + third-person hold).
  Engaged **by default at scene start when the player is a participant** (the runtime calls
  `EngageDefaultPlayerLock`; a scene opts out with `lockPlayer:false`), and also by the explicit
  `osf.control.lock` action. Ref-counted per scene, ledger-tracked so it auto-releases on any end.
- **Papyrus** (`src/Papyrus/OSFScript.*`) — natives bound via GameVM at kPostDataLoad, re-bound every
  load (VM is rebuilt).
- **Save-safety** (`src/Serialization/SaveSafety.*`) — `GraphManager::StopAll` drops ALL scene/graph
  state on a world-replacing load (SaveLoadEvent begin sink + TESLoadGameEvent backstop + manual).
- **Startup** (`src/main.cpp`) — logs game-version vs RE build, loads packs+scenes+sound pools, inits
  SoundService + applies `Settings`, registers the runtime with GraphManager, emits a feature report.

### Layer B - scene runtime
- **SceneRegistry** (`src/Registry/SceneRegistry.*`) - loads the unified `*.osf.json` scene defs (nodes +
  edges + roles + loop/timer + cue/action/sound/camera tracks) + validation (`GetSceneLoadErrors`); also
  carries the `stripActors`/`lockPlayer` default-mechanism opt-outs (top-level → per-role). The `sound`
  lane also accepts a ladder-sugar OBJECT `{ role?, spec, repeat?, marks }` (shared defaults that expand
  to one entry per mark, appending each mark's tag(s) to the base `spec`). `marks` is either GROUPED —
  `{ "low":[0.1,0.3], "loud":[0.8] }` (key = tag(s) to append, value = positions; terse for repeated
  tiers) — or an ARRAY of `[at, tag…]` pairs / bare `at` / `{ at, … }` overrides (ordered, heterogeneous).
- **SoundRegistry** (`src/Registry/SoundRegistry.*`) — loads tagged sound pools from
  `Data/OSF/**/*.sounds.json` (`{ schema, pools:[{ name?, tags[], clips[] }] }`). A `sound`/`osf.voice.play`
  spec starting with `$` is a pool query (`$tag,tag,…`, all-of); the runtime resolves it to ONE clip by
  weighted-random AT FIRE TIME (`SceneRuntime::PlaySound`), so a per-loop/repeated cue re-rolls (the
  per-hit variation a single literal path can't give). A `{gender}` token in the query is substituted
  from the role actor (`male`/`female` via `Matchmaking::ActorGenderTag`; empty→the tag drops out, i.e.
  any gender), so one spec like `$seduce,{gender},moan,loud` plays the matching-gender pool. Plain specs
  stay literal. Content-neutral; reloaded by `ReloadPacks`.
- **SceneRuntime** (`src/Scene/SceneRuntime.*`) - generational handle table, lifecycle (`Fire`), navigation, timed-mark decode (`OnTimedMarks`, lane-ordered action→camera→sound→cue)
  `osf.*` action dispatch (`RunAction`), and the **per-handle undo ledger** (reverses every reversible
  mechanism in reverse order, once, idempotently, on ANY termination). Talks to Layer A only via its public surface, to Layer C via services.
  **Scene-start defaults** (`EngageDefaultPlayerLock` / `StripDefaultActors`, called once per start in each
  `Start*` funnel): lock the player's input when they participate (opt out `lockPlayer:false`) and strip
  every participant's apparel (opt out `stripActors:false`); both ledger-tracked so they auto-reverse on end.
  The caller resolves each opt-out — from the `SceneDef` for a def scene, the `PackPolicy` for a pack — and
  passes the booleans in; a files scene has no field, so both stay on.
- **SceneEventRelay** (`src/Scene/SceneEventRelay.*`) - token registry + async C++->Papyrus dispatch of
  the `OSFEvent:SceneEvent` struct.
- **Matchmaking** (`src/Matchmaking/Matchmaker.*`) — unified candidate pool over `SceneRegistry` defs;
  deterministic role-binding; `roles[].filters` gender/keyword/race with
  `"Plugin.esm|0xID"` form-refs resolved at load (RE-sensitive — needs in-game verification).

### Layer-C - services 
- (`src/UI/FadeService.*`, `src/Equipment/EquipmentService.*`, `src/Audio/{SoundService,WwiseBackend}.*`, Player/Camera locks) -
  content-neutral *mechanisms* with NO scene knowledge; each prologue-gates its engine calls and
  self-disables on mismatch. Whether a mechanism runs is driven by the scene/API alone — there is no
  user-settings feature toggle; user settings (hotkeys, `logLevel`, HUD toggles) live in OSF UI's
  in-game settings menu (`src/API/UISettings.cpp` registers the schema over the bridge; the old
  `Data/OSF/settings.json` is no longer read).

## Logging

All logging goes through `REX::{TRACE,DEBUG,INFO,WARN,ERROR}` (commonlib → spdlog → the SFSE log
file). Two rules keep it cohesive:

- **Tier by audience.** The default level is build-driven — `Info` in the shipped `releasedbg` build,
  `Debug` in debug builds — so anything below `Info` is invisible to users by default.
  - `ERROR` — an operation the user/author can fix failed (bad setting value, native bind failed,
    scene/pack failed validation, referenced asset missing, hook couldn't install).
  - `WARN` — degraded but continuing (unsupported game version, a feature self-disabled on prologue
    mismatch, unknown settings key).
  - `INFO` — a few lifecycle milestones only: load banner, the `FEATURE:` report, registry load
    summaries, save-load teardown, `Load complete`, and scene **start/end** (id + handle).
  - `DEBUG` — author-debugging detail: scene advance/navigate, action dispatch, matchmaking results,
    sound resolution, mechanism engage/release (lock/strip/fade/equip), solo clip play/stop.
  - `TRACE` — engine/RE internals: per-frame/per-mark, hook plumbing, RE offsets, AF/BA2/GLTF decode
    steps, anchor/placement geometry, sync/frame-lock, ledger push/pop.
- **Tag by subsystem.** Every line starts with one bracketed tag so the log greps cleanly:
  `[Boot] [Feature] [Anim] [Scene] [Registry] [Sound] [Match] [Papyrus] [API] [Audio] [Camera] [Player]
  [Input] [Hotkey] [Equip] [Weapon] [UI] [Save] [Config]`. Don't restate the subsystem in the message text.
  (`[API]` = the native C++ inter-plugin API in `src/API/`; see docs/RFC-native-api.md.)

A dev gets the full firehose without recompiling via the in-game OSF UI settings menu — OSF
Animation › Logging › Log level → Trace (handled in `src/API/UISettings.cpp`, persists across
launches). The crash handler (`src/Util/CrashHandler.cpp`) deliberately bypasses spdlog — never
route it through REX.