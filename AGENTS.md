# OSF Animation

SFSE plugin - the **engine** of **OSF**, a scene framework for Starfield (NAFSF playback lineage). It hosts native playback, sync, the shared clock, anchoring, a clip/pack registry, and the **scene runtime** (graphs of nodes with cues/actions/callbacks/navigation). The engine stays
**content-neutral**: it provides policy *mechanisms* (control/camera lock, fade, equipment-strip,
voice/sound, camera hold);

## Build

- **Native (C++/DLL):** build both DLLs + deploy with `xmake` (mode `releasedbg` — `xmake f -m releasedbg`,
  then `xmake`). This is the only thing an agent compiles.
- **Tests:** `xmake test` builds and runs the ten engine-free C++ suites (persistence broker,
  native scene-event callbacks, browser/engine API boundary, generated GLTF/GLB/gzip import fixtures,
  frame clock/clip specs, clip-duration cache, additive pose math, shared scene launcher, sound registry, scene registry — the registry pair run from
  `test/fixtures`). Run it after native changes; exit code is the failure count. The view has its
  own suite: `npm --prefix extensions/browser/ui run verify` (typecheck + build + vitest).
- **Smoke bench:** `pwsh -NoProfile -File tools/test-smoke.ps1` builds production, tests native/browser,
  and checks release archives with retained evidence. `-RunGame` adds the sibling harness's private
  Animation scenario (47 live assertions). `-Hosts Deployed` tests the currently deployed UI/Settings
  hosts; the default uses the pinned releases. `-Plan` is read-only. Runtime instrumentation requires `--animation_smoke=y`,
  defaults off, and must never be deployed to the normal mod or packaged. The bench uses separate
  `build/smoke-native` output and restores production configuration. See `docs/SMOKE_TESTING.md`.
- **UI build:** `extensions/browser/ui` uses a local Vite/Octane build. Run `npm ci`
  there before UI scripts. The retired OSF UI authoring CLI is no longer required.
- **UI / Settings SDKs:** vendored from *released* tags, never a sibling's working tree:
  `OSF UI/sdk/OSFUI.h` + `osfui.d.ts` (UI v2.0.0, native API 2.0) and
  `OSF Settings/sdk/OSFSettings{,_Diagnostics}.h` (Settings v1.0.0). `sdk-lock.json` records each
  file's source tag, commit, and LF-normalized SHA-256; `tools/Test-SdkLock.ps1` checks it (smoke
  stage `sdk`). Re-vendor with `git show <tag>:sdk/<file>` and update the lock in the same change.
  The UI API has no readiness callback: the view fetches its data on document init, and retained
  settings go out via `SetState` immediately. OSF UI renders views; OSF Settings owns values, hotkeys,
  launchers, and issues. The schema ships under `dist/SFSE/Plugins/OSF/Settings/schemas`.
- **Release archive:** `packaging\build-archive.ps1` (build → stage FOMOD → verify → zip; see
  `docs/PACKAGING.md`). Output: `packaging\out\OSF Animation v<version>.zip`.
  Packaging uses private staging, never MO2. `-SkipBuild` requires a current build receipt;
  PSC/PEX freshness and compiled native signatures must match. `-ResultPath` identifies
  the verified ZIP manifest. Ordinary `xmake` remains the build-and-deploy workflow.
- **Papyrus (`dist/Scripts/Source/*.psc` → `*.pex`): recompile after editing a `.psc`, THEN `xmake`
  to deploy the fresh `.pex`.** A stale `.pex` (source edited but not recompiled) makes any new/changed
  native fail to bind at link time — e.g. `error: Native static function X could find no matching
  static function on linked type OSF. Function will not be bound.` Compile with the CK Papyrus
  compiler (`OSFTypes.psc` sits in the import path so `OSFTypes:*` signatures resolve):
  ```powershell
  # Canonical invocation; keep it aligned with .vscode/tasks.json and the packager.
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
- **Clip durations** (`src/Serialization/ClipDurations.*`, `ClipDurationCatalog.*`) — playback records
  decoded durations in memory only. Explicit catalog scans load/probe/prune and persist snapshots on
  a background worker; no cache IO or background work occurs from recording or lookup. Records made
  after a save snapshot remain pending until the next requested scan. The cache path/format and engine
  services ABI are unchanged.
- **Graph** (`src/Animation/Graph.*`) — per-actor sampler/stamper across two hooks: `Sample()`
  (advances time/stage, keeps rig binding warm) + `StampPose()` (samples ozz pose ONCE/frame, writes
  engine flat rig buffers, NiTransform ROW layout — do NOT transpose).
- **GraphManager** (`src/Animation/GraphManager.*`) — graph map keyed on `TESObjectREFR*`; 
  owns the two vtable hooks. `PlaySynchronized` is the shared playback entry. `StopAll` drops all state on load (releases the persistent AI-driven flag).
  Compose-root pin + cull-sphere + BSFadeNode fade suppression live in `Hook_ModelNodeUpdate`
- **FrameClock** (`src/Animation/FrameClock.h`) — owner-token clock so subdivided updates advance 1×/ frame; defines `SyncGroup`.
- **Frame maintenance** (`src/Maintenance/FrameMaintenance.*`, `src/Util/NativeMainThreadQueue.*`) — hooks Main's normal frame-update call at `99488+0x2D6` (target `99406`, cadence and pause/resume verified in-game on 1.16.244), running after the original. Reduced loading updates bypass this call. Drains one finite FIFO command batch, then ticks camera, fade, subtitles, and graph maintenance. Worker producers use `NativeMainThreadQueue::Post`; related playback marks/end commands use `PostBatch`. Posts made during a drain wait for the next update. Anchored NPC animation-driven flags are maintained once per game frame here, with save-window suppression. Sampling and stamping remain in their animation hooks.
- **PlaybackSession** (`src/Animation/Scene.*`) — pure Layer-A: shared clock + participant graphs + anchor +
  per-stage {files, placements, timer, loops, blend}. Auto-advances on timer/loop-target.
- **Player/Camera locks** (`src/Player/PlayerInputLockService.*`, `src/Camera/CameraService.*`) —
  standalone locks (input-disable + AI-driven + third-person hold).
  Engaged **by default at scene start when the player is a participant** (the runtime calls
  `EngageDefaultPlayerInputLock`; a scene opts out with `lockPlayer:false`), and also by the explicit
  `osf.control.lock` action. Ref-counted per scene, ledger-tracked so it auto-releases on any end.
- **Papyrus** (`src/Papyrus/OSFScript.*`) — natives bound via GameVM at kPostDataLoad, re-bound every
  load (VM is rebuilt).
- **Save-safety** (`src/Serialization/SaveSafety.*`) — `GraphManager::StopAll` drops ALL scene/graph
  state on a world-replacing load (SaveLoadEvent begin sink + TESLoadGameEvent backstop + manual).
- **Shared persistence** (`src/API/OSFPersistenceAPI.h`, `src/Serialization/Persistence{Broker,Host}.*`) —
  public C ABI for per-plugin record streams in one atomic `.osf` sidecar (`OSFP` container); deterministic/bounded broker, load remap/delete forwarding, save/load/delete hooks, and internal `OSFA` client. See `docs/PERSISTENCE_API.md`.
- **Startup** (`src/main.cpp`) — logs game-version vs RE build, loads packs+scenes+sound pools, inits
  SoundService + applies `Settings`, registers the runtime with GraphManager, emits a feature report.

### Layer B - scene runtime
- **SceneLauncher** (`src/Scene/SceneLauncher.*`) — shared registered-scene launch path for Papyrus,
  native API, and matchmaking: cast validation, stage selection, anchor resolution, runtime dispatch,
  and optional post-start speed. Adapters retain compatibility choices: Papyrus rejects out-of-range
  linear stages and keeps authored speed; native starts fall back to entry and may override speed.
  Matchmade starts use the picked cast order and authored entry/speed. Runtime retains role binding,
  role filters, admission, playback, and lifecycle ownership. Ad-hoc plans still use `StartFromPlan`.
- **ContentRegistry** (`src/Registry/ContentRegistry.*`) - loads the unified `*.osf.json` scene defs (nodes +
  edges + roles + loop/timer + cue/action/sound/camera tracks) + validation (`GetSceneLoadErrors`).
  Each load also publishes a **per-file import record** in the snapshot (`ContentFileStats` / `FileStats()`):
  one per discovered file *including files that contributed nothing*, carrying counts, size, parse time
  and the load problems attributed to it (`ProblemSink` pairs every problem line with its owning file, so
  attribution never depends on parsing the message text). `LoadErrors()` is unchanged. The browser's
  IMPORTS panel is the consumer, over `osf.animation.imports.get`. Also
  carries the `stripActors`/`lockPlayer` default-mechanism opt-outs (top-level → per-role). A multi-scene
  file's top-level `roles` is read by JSON type: an ARRAY is a default cast inherited by scenes omitting
  `roles`; an OBJECT is a file-local registry of reusable role TEMPLATES that scene `roles` entries
  reference by exact id — either a bare string (copy as-is) or an `{ "id", ...overrides }` object
  (JSON-merge-patch over the template: scalars replace, filters/offset/equip merge by key, arrays
  replace, null removes; `gender`/`filters.gender` aliased so one override drops the other path),
  mixed freely with inline objects. A template omitting `name` defaults it to the id; a registry is
  NOT a default cast — omitted `roles` still infers. Templates validate at file load (malformed
  rejects the file); unknown/empty/non-string `id`s or duplicate EXPLICIT names reject only the
  scene. Automatic (template-derived) names are numbered past collisions — `["m","m","f"]` →
  m, m2, f — while explicit names are kept exactly and reserved first. The `sound`
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
  **Scene-start defaults** (`EngageDefaultPlayerInputLock` / `HideDefaultApparel`, called once per start in each
  `Start*` funnel): lock the player's input when they participate (opt out `lockPlayer:false`) and strip
  every participant's apparel (opt out `stripActors:false`); both ledger-tracked so they auto-reverse on end.
  The caller resolves each opt-out — from the `SceneDef` for a def scene, the `PackPolicy` for a pack — and
  passes the booleans in; a files scene has no field, so both stay on.
- **SceneEventRelay** (`src/Scene/SceneEventRelay.*`) - synchronous native API callback dispatch at
  the authored runtime mark, followed by the existing token registry + async C++->Papyrus dispatch
  of the `OSFEvent:SceneEvent` struct. Native registrations live in
  `src/API/NativeSceneEventRegistry.*` and survive world replacement.
- **Matchmaking** (`src/Matchmaking/Matchmaker.*`) — unified candidate pool over `ContentRegistry` defs;
  deterministic role-binding; `roles[].filters` gender/keyword/race with
  `"Plugin.esm|0xID"` form-refs resolved at load (RE-sensitive — needs in-game verification).

### Layer-C - services 
- **Browser addon** (`extensions/browser/src/API/BrowserSession.*`, `BrowserSessionQueue.h`) — separate optional
  `OSF Animation Browser.dll`, built without private engine implementation. It consumes scene ABI 1.3
  and engine services ABI 1.0 (`src/API/OSFEngineAPI.h`). Owns browser/wheel presentation, picking, pins,
  preferences and hotkeys; see `docs/BROWSER_ADDON.md`. It is an
  OSF UI consumer. Native shown/hidden callbacks own browser lifetime; copied commands and
  pointer input drain on the frame queue with a generation check. Hide/world replacement
  invalidate pending work. `UIBridge` owns game behavior; document initialization replays data.
  Missing addon or OSF UI is normal; an incompatible installed UI or invalid assets reports through Mod Issues.
- (`src/UI/FadeService.*`, `src/Equipment/EquipmentService.*`, `src/Audio/{SoundService,WwiseBackend}.*`, Player/Camera locks) -
  content-neutral *mechanisms* with NO scene knowledge. Typed engine operations use CommonLibSF
  directly; private ABI/layout access (fade posting and free-camera offsets) retains its compatibility gates. Whether a mechanism runs is driven by the scene/API alone — there is no
  user-settings feature toggle; user settings (hotkeys, `logLevel`, HUD toggles) live in OSF Settings's
  in-game menu (`src/API/EngineSettings.cpp` subscribes to the shipped schema; the old
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
  (`[API]` = the native C++ inter-plugin API in `src/API/`; see `docs/API.md`.)

A dev gets the full firehose without recompiling via the in-game OSF Settings menu — OSF
Animation › Advanced - Diagnostics › Log level → Trace (handled in `src/API/EngineSettings.cpp`, persists across
launches). The crash handler (`src/Util/CrashHandler.cpp`) deliberately bypasses spdlog — never
route it through REX.

## Git workflow

- Commit only when the user explicitly requests it; completion, verification, and handoff do not authorize a commit.
- Keep commits focused, and verify the staged diff so unrelated user changes are never included accidentally.
