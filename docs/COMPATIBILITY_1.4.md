# Published 1.4 compatibility in 2.0

The 2.0 candidate includes a bounded adapter for the **published v1.4.0** contract.
It translates at import/API boundaries and uses the current preparation, playback,
ownership and cleanup runtime. It does not run a second copy of the old engine.
Offline verification does not yet qualify existing compiled mods in the game VM.

## Supported boundaries

- Schema-1 `*.osf.json`: bare clips, linear stages (including array shorthand and
  stage-scoped lanes), scene containers, graph entries/local routes, `linearStages`,
  role inference/templates, and single-inline-scene `use` references across files.
  Old `stripActors`, `lockPlayer`, `playerControl` and `inPlace` policies translate
  only in schema 1. Conflicting old/new policy fields are diagnosed, not guessed.
- The original scene ID starts its entry. Interior nodes get deterministic global
  `osf.compat14/...` IDs and are unlisted so ordinary tag matching does not select
  them as extra scenes. Those IDs are implementation addresses, not new authoring IDs.
  Explicit catalog readers can still see them. Schema-2 authoring remains strict.
- Legacy stage getters/setters use the original container mapping; graphs without
  `linearStages` report -1 and reject stage jumps. Starting at stage 0/nonpositive
  means authored entry. Positive out-of-range starts fail for Papyrus and fall back
  to entry for native callers, matching 1.4. In-progress stage jumps and edges keep
  the source publication pinned across ReloadPacks.
- Papyrus callbacks for schema-1 scenes retain local node names, including `#sN`.
  Current native callbacks and GetNode retain global addresses. The 2.0 transition
  event remains available; consumers should switch on recognized event types.
- All 44 published OSF/OSFAdvanced native signatures are retained. SceneOptions
  again exposes `StripMode`, `LockPlayerMode`, `PlayerControlMode`, `InPlaceMode`.
  An explicit canonical 0/1 override wins; otherwise the legacy member is read.
- The exported scene request serves a separate frozen ABI-1.2 vtable, including
  its earlier 1.0/1.1 request/prefix compatibility. Old field names map to current
  start options; absent appended fields inherit. Current clients receive ABI 2.0.
  The frozen header is namespace-isolated from `v1.4.0:src/API/OSFSceneAPI.h`.

## Deliberate limits

**Admission is asynchronous.** A nonzero start reserves a handle; playback begins
only after preparation commits. Subscribe before starting, wait for SCENE_BEGIN,
and handle SCENE_END without begin. A true stage/navigation result means accepted,
not finished. Ended participant rosters follow 2.0 bounded retention; do not retain
old handles indefinitely for roster queries. A mod relying on synchronous playback or issuing immediate chained
commands may need an update. The shim does not bypass preparation to emulate that.

This is not compatibility for unpublished 1.5 development contracts: scene ABI
1.3-1.7, services ABI 3.x, GetSceneId and scene/node-pair APIs remain unsupported.
Catalog/provider data stays schema 4/2. Old settings files, combined UI hosts,
alignment files and removed generated content IDs are not migrated by this layer.
Install the 2.0 engine and matching optional Browser/hosts normally.

Current registry bounds and validation still apply. Invalid local references,
ambiguous policy fields, generated-ID collisions, unsupported/chained `use` targets,
and mismatched casts are diagnosed. A malformed legacy conversion can reject its
file. Save/VM struct migration and third-party native binary behavior need live
qualification; no claim of universal drop-in compatibility is made.

## Evidence and remaining game tests

`test/compat14` contains five unchanged positive JSON fixtures from the published
v1.4.0 tag, plus focused linear/graph/use fixtures. The production registry suite
checks translation, timing, local routes, shared casts, role templates, reload
pinning and bad declarations. Launcher tests check the legacy stage rules. The
Browser/engine boundary suite exercises the production adapter through the frozen
vtable, prefix sizes, renamed options, stage calls and the last published slot.

The candidate release evidence is recorded in
[2.0 acceptance](RELEASE_2.0.0_ACCEPTANCE.md). Before releasing compatibility:

1. Install an unchanged real 1.4 animation pack and its already-compiled consumer
   PEX. Exercise all four legacy SceneOptions members, both forced values, inheritance,
   and save/reload of a script holding the old struct. Check native binding logs.
2. Start linear content at a nonzero stage; advance, jump backwards, navigate a
   graph, and verify local node names in callbacks. Reload packs while active and
   confirm routes and stage jumps keep the old definition until scene end.
3. Load an unchanged native consumer built against ABI 1.2. Exercise start options,
   participant queries, stage control and stop. Check failed preparation, immediate
   cancellation, and commands issued while preparation is pending.
4. Verify camera/input/equipment restoration on normal end, cancellation, failed
   start and world replacement with legacy and schema-2 scenes installed together.
