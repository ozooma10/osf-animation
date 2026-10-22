# Simplification and performance review — 2026-10-22

Read-only pass over the whole repo at `dev` 493a4ba (clean tree). Nothing was changed.
Five parallel reviews (core playback, scene runtime/registries, API/services/Papyrus,
browser addon + UI, tooling/build/tests) plus independent verification of every item in
tiers 0–2 and the top of tier 3 at the cited lines. Line numbers are from this commit.

## Applied on 2026-10-22 (uncommitted)

Built (`releasedbg`, smoke off, deployed to MO2), `xmake test` 14/14 passed, UI `verify` 166/166 passed,
`test/ReleaseValidationTest.ps1` and `test/SmokeBenchTest.ps1` passed. Not yet verified in game.

- **Tier 0:** #1 coverage map + doc count; #2 one-shot fallback WARN for `emitter: role`; #3 retracted
  (false positive); #4 and #5 unified in a new side-effect-free `tools/ReleaseRules.ps1`
  (`Test-ReleaseForbiddenEntry`, `Test-InstrumentedDll`) used by the bench, the packager and `Run-Game.ps1`.
- **Tier 1 applied:** #1 input hook, #2 fade tick, #3 drain-before-sink (also stops sinkless marks piling
  up; `DrainFiredMarks` keeps its replace-not-append contract), #4 clock half only (one steady-clock read
  per Sample via a new `PlaybackSession::Advance(token, dt, nowMs)` overload), #7 `FindNode`/`LinearStageOf`,
  #8 lane sort only when unordered + cue pointers + allocation-free trigger match, #9 both struct lookups,
  #10 debug gates on the Fire/cue/action/sound lines and the matchmaking describe calls, #11 role
  resolution only, #14 focus half only (foreground check every 250 ms), #15 `ComposeFormID`,
  #16 no catalog re-push on pin toggle (reset still resyncs), #18 haystack + preference precomputed in
  the memoized item build (no regex in the comparator), #19, #21, #22. New shared helper:
  `Util::IEquals` in `shared/Util/StringUtil.h`.
- **Tier 1 deliberately not applied:** #4 walk-skip, #5, #6, #13 and #20 (medium risk); #11 SlotView
  refactor and #17 (low–medium); #12 pin-trig cache (negligible gain for new cached state at three write
  sites); #18 search debounce and shared `visiblePlayables` memo (behavioral change to typing feel; the
  per-item cost is gone). Per-branch `RunAction` debug lines left ungated (cheap scalar args).

## Tier 2 applied on 2026-10-22 (uncommitted, after d4832cf)

Built (`releasedbg`, deployed to MO2), `xmake test` 14/14, UI `verify` 166/166, Skyrim importer
51/51 plain Python (2 Blender-only suites skipped there) and 22/22 inside Blender 5.1. Not verified in game.

- **Applied:** #1 (`GLTFImport::TryCached` / `AFImport::TryCached` probed before the BSResource read),
  #2 (library entries feed the load's shared installed cache; `current_path()` once per resolve; a
  process-wide Data-dir cache was rejected because suites chdir), #4 (per-actor facts with memoized keyword
  hits plus a per-def role×actor table; `RoleAccepts` shares the same filter), #5, #6 header-only `.af`
  probe read (BA2 index left for later), #8, #9 AF half only (single rotation per key, reserved unfolds),
  #11 (table CRC with the same polynomial; records written straight into the file buffer, format unchanged),
  #12, #15–#19, #21, #24 (bone order resolved once per hierarchy; preflight hands decoded clips to the
  worker).
- **Not applied:** #3, #7, #10, #22, #23 (medium risk); #6 BA2 name index; #9 GLTF lowercasing;
  #13 and #14 (negligible: parser `reserve`s, a few-pointer copy per rare API call); #20 (one lookup per
  stage per catalog refresh); #25 (needs a regeneration run against game archives to diff the committed
  packs); #26 (exact user-log subfolder unconfirmed).

Frequency legend: **per frame** = animation hook / frame drain / input hook; **per mark** =
each fired cue/action/sound/camera; **per start** = scene start, handoff, node transition;
**load** = pack load, reload, catalog build, save/load.

---

## Tier 0 — live defects found in passing (fix regardless of this pass)

1. **Smoke bench coverage gate fails today.** `xmake.lua` registers 14 `add_tests` targets, `test/smoke/coverage.json` maps 12 (missing `osf-actor-state-test`, `osf-preparation-test`). `Test-SmokeCoverage` (`tools/SmokeBench.ps1:304-308`) throws on the mismatch, so `test-smoke.ps1`'s `coverage` stage fails and `native` is skipped. `docs/SMOKE_TESTING.md:78` still says "twelve". Fix: add both suites to the map, fix the doc.
2. **`emitter: role` sounds silently fall back to the listener.** `SoundService::PrepareRoleEmitter` (`src/Audio/SoundService.cpp:149-153`) is a stub returning 0. The fallback itself is documented and deliberate (`docs/SCENE_SCHEMA.md` "Sound emitters": the unverified Wwise registration entry point was removed), but nothing in the log said so. **Applied:** a one-shot `[Sound]` WARN on first fallback and an accurate `listener (role fallback)` debug line in `src/Audio/SoundPlayback.cpp`.
3. ~~**Papyrus `StartSceneRoles` and `StartSceneRolesEx(…, None)` differ on speed.**~~ **Retracted on follow-up.** Registered scenes carry no authored speed: `SceneRuntime::Start` seeds `a_over.speed.value_or(1.0f)` (`src/Scene/SceneRuntime_Graph.cpp:475`), so an unset speed and the `SceneOptions` default of 1.0 are the same. No change needed.
4. **Forbidden-release-entry regex has two drifting copies.** `packaging/ReleaseValidation.ps1:75` (`TestHarness`, `node_modules`) vs `tools/SmokeBench.ps1:335` (`AnimationTestHarness`, no `node_modules`). `OSFSettingsTestHarness.json` is rejected by one and not the other. Export one pattern from `SmokeBench.ps1`.
5. **Instrumented-DLL detection disagrees by encoding.** `packaging/ReleaseValidation.ps1:10-16` checks UTF-8 and UTF-16; `test/loop-globals/Run-Game.ps1:150` checks ASCII only. One `Test-InstrumentedDll` for both.

---

## Tier 1 — per-frame / per-input hot paths (verified)

### Engine (`src/`)

1. **Input hook interns a `BSFixedString` every frame even when OSF is idle.** `src/Input/InputService.cpp:184` computes `MenuOwnsInput()` before the `(active || capture || routing)` gate; `ui->IsMenuOpen(RE::Console::MENU_NAME)` (`:173`) builds a `BSFixedString` from a `string_view` each call (pool hash/intern/release) plus two singleton reads, on the input thread. Fix: `static const RE::BSFixedString kConsoleMenu{…}` and evaluate lazily after the gate. Risk low.
2. **`FadeService::Tick` takes a mutex every frame before reading the atomic deadline; the CAS loops are redundant.** `src/UI/FadeService.cpp:124-136`, `:75-83`. Every writer of `releaseAtMs` is already under `transitionLock`. Fix: peek the atomic first and return when 0 / not due; lock only to post; replace CAS with plain stores. Risk low.
3. **Every `Sample` copies the whole `PlaybackSink` (five `std::function`s) under `_sinkLock` before checking whether any marks fired.** `src/Animation/GraphManager.cpp:1485-1495` → `GetPlaybackSink` (`:244-250`) returns `std::optional<PlaybackSink>` by value; called ~7×/frame per scene participant. Fix: drain first (cheap swap), return if empty, fetch the sink only when marks exist, or cache `hasTimedMarks` on the session at publish like `prepareStages` (`:472`). Also `DrainFiredMarks` (`src/Animation/Scene.cpp:280-285`) swaps into a fresh vector so capacity is lost each drain. Risk low.
4. **`Graph::Sample` re-walks `refr → loadedData → BSFadeNode → BGSModelNode` on every call before the cached-identity early-out, and calls `steady_clock::now()` three times.** `src/Animation/Graph.cpp:428` → `ResolveAndBind` `:207-226` (a `BSReadWriteLock` acquire + two `NiPointer` refcount RMWs per call); clock at `:395-396`, `:398` (one-arg `FrameClock::ShouldAdvance`, `src/Animation/FrameClock.h:21-26`, its only caller), and `src/Animation/Scene.cpp:65-66` or `Graph.cpp:443-444`. Fix: compute `nowMs` once and pass it through (delete the one-arg overload); re-walk the chain only on the clock-owner call or when `cachedModelNode` is null. Risk low (clock) / medium (walk; rebuild detection latency becomes ≤1 frame).
5. **Both hooks take `stateLock` shared for every actor update and every skeleton compose game-wide once any graph exists.** `src/Animation/GraphManager.cpp:1556-1557`, `:1585-1588`. The `graphCount` gate only helps when idle; during a scene, every NPC's ~7 updates/frame and every `BGSModelNode::Update` in the loaded area hit the same SRW cacheline, and each unique holder (`PlayPrepared` admission loop `:477-565`, `RemoveFadedGraph`, `StopAll`) stalls all actor animation. Fix: a small lock-free membership snapshot (atomic refr/modelNode slots written under the unique lock); the existing re-verification under the graph lock handles ABA. Risk medium; magnitude depends on NPC density.
6. **A graph with no published stamp target makes every skeleton compose in the world take that graph's unique lock and run `ResolveAndBind`.** `:1591-1600`. For live actors the window is <1 frame; for a stranded graph (`BeginFadeOut` → `InvalidateBinding` nulls the target) it lasts the fade ramp + 2 s or up to 5 s. Fix: skip recovery for graphs with `removalQueued` or `lastSampleMs` older than `kClockOwnerStaleMs`. Risk medium.
7. **`SceneDef::FindNode` / `LinearStageOf` allocate N+1 lowercase strings per call and sit on the per-mark path.** `src/Registry/ContentRegistry.cpp:2328-2348`; callers `src/Scene/SceneRuntime_Graph.cpp:117,185` (2× per mark batch), `src/Scene/SceneRuntime_Dispatch.cpp:130,160,217,288` (4× per node enter/exit), `src/API/CatalogBuilder.cpp:16,119` (O(stages × nodes) per def per catalog build). Linear scenes have one node per stage. Fix: lowercase ids at parse and compare allocation-free, or build a node index once. Risk low.
8. **`OnTimedMarks` copies and `stable_sort`s the `FiredMark` vector (one `std::string` per mark) and copies `CueEntry` structs per batch; the trigger-edge loop lowercases `cue.id` and every `e.trigger` under `_lock` per cue.** `src/Scene/SceneRuntime_Graph.cpp:93-95`, `:132`, `:187-195`; `Navigate` `:767-770`. The parser already resolves every trigger to a cue on the node (`ContentRegistry.cpp:1508-1524`). Fix: four lane passes over `a_marks` without copying, `const CueEntry*` in `cues`, `SceneEdge::triggerCue` index set at parse. Risk low.
9. **`SceneEventRelay::PackPayload` rebuilds a 14-entry lowercase name→slot map per dispatched event** (`src/Scene/SceneEventRelay.cpp:28-41`, ~30 allocations before the VM call); **Papyrus `ReadSceneOptions` does the same per `Start*` call** (`src/Papyrus/OSFScript.cpp:73-81`). Fix: cache a slot array keyed on `proxy->type.get()`, reset in `Clear()` / on VM rebuild. Risk low–medium.
10. **`REX::DEBUG` formats before the level check, so per-mark dispatch lines and matchmaking describe calls pay full cost at the shipped Info level.** `lib/commonlibsf/lib/commonlib-shared/include/REX/LOG.h:24-28` (`std::vformat` then `Log`). Hot sites: `src/Scene/SceneRuntime_Dispatch.cpp:55,104-105,179-201,238-239,319-398`, `src/Audio/SoundPlayback.cpp:39-40`, `src/Matchmaking/Matchmaker.cpp:341-354` (`DescribeAnchorSearch` calls `GetFormEditorID` on the engine per query; `:125` already notes the problem). Fix: a project-side `if (Log::Enabled(Debug))` guard (level is owned by `src/API/EngineSettings.cpp`). Risk low.
11. **`SnapshotSlot` copies the roster per dispatch step; `ResolveRoleActor` lowercases every role name per call; `LifecycleValid` checked twice per boundary.** `src/Scene/SceneRuntime.cpp:193-207`; `SceneRuntime_Dispatch.cpp:126,156,208,284` (4 dispatchers), `:257-274`, `:39-50,81-92` vs `:136,166,227,306`. Fix: one `SlotView` from `Fire` into the dispatchers; resolve role→index once per node (or lowercase `SceneRole::name` at parse). Risk low–medium (keep the per-entry `LifecycleValid`).
12. **`Hook_ModelNodeUpdate` recomputes pin trig per compose per pinned participant** (`src/Animation/GraphManager.cpp:1619-1622`, `:1639-1640`; inputs change only at `Graph.cpp:412`, `GraphManager.cpp:550,984`). Fix: cache `pinWorld/cosH/sinH` at the write sites. Small gain, risk low.
13. **`StampPose` unpacks every skeleton joint SoA→4×4 though only bound joints are read** (`src/Animation/Graph.cpp:518`; face/eye/morph never bind, `hands` mask binds ~40/200). Fix: unpack only SoA groups touched by `binding`. Risk medium (verify `PrepareBlendSource`/`Consume`).
14. **`CameraService::DriveSceneOrbit` issues ~6 user32 syscalls per frame while an orbit is live** (`src/Camera/CameraService.cpp:648-656`: `GetForegroundWindow`, `GetWindowThreadProcessId`, 4× `GetAsyncKeyState`). Fix: cache focus for N ms; optionally capture WASD in the input hook. Risk medium for the key half, low for focus.
15. **`ComposeFormID` lowercases every plugin filename in the load order per call** (`src/Util/FormRef.h:55-75`); runs per keyword/race ref per role at load (`ContentRegistry.cpp:919,924,1608,1617,2532`), per `osf.equipment.equip` fire (`SceneRuntime_Dispatch.cpp:360`) and per gear override row per start (`GearRegistry.cpp:234,257`). Fix: allocation-free case-insensitive compare, or memoize plugin→index (load order is fixed). Risk low.

### Browser addon (`extensions/browser/`)

16. **Every wheel-pin toggle re-serializes and re-ships the whole catalog, then forces a native cast re-match.** `src/API/UIBridge.cpp:847-850` → `PushCatalogUpdate` `:1089-1096` (full `BuildCatalog`, sort, dump, chunked transfer); UI already applied pins optimistically (`ui/src/app/reducer.ts:48-56`), and `catalog/received` nulls `castMatch`, re-triggering `osf.animation.castMatch` (`ui/src/app/controller.ts:390-393`). Fix: drop the push from `OnWheelSet` (or keep `castMatch` when only pins changed). Risk low.
17. **One pack reload builds the main catalog three times and re-parses the engine snapshot three times.** `UIBridge.cpp:370-396`, `:1228-1236`, `ClipDurationCache.cpp:273-275` (unconditional `Invalidate` + push after scan), `EngineClient.cpp:46-61`, `controller.ts:238-246`. Fix: skip the immediate push on `ContentReloaded` (UI re-requests on `reloadResult`); only invalidate/push from `Scan` when `revision` advanced. Risk low–medium.
18. **Per keystroke, the playable filter/sort pipeline runs 2–3×, rebuilds a haystack string per item and runs a regex inside the sort comparator.** `ui/src/app/selectors.ts:215-221`, `:187-208` (`playablePreference` regex + tag scans per comparison ≈ 150k tests per sort; `fitsKeyedAnchor` 2× per comparison), `:241-246`; `BrowsePanel.tsx:219-229`; `controller.ts:395-412`. No debounce on `setSearch`. Fix: precompute `hay`/`preference` in `buildPlayableItems`, one memoized `visiblePlayables(state)` shared by the three consumers, 50 ms debounce. Risk low (covered by `tests/app.test.ts`).
19. **`PlayableGroups` builds each group with O(k²) array copying** (`ui/src/features/browse/BrowsePanel.tsx:121-125`: `groups.set(key, [...(groups.get(key) ?? []), entry])`; ~1.6M element copies per render for a 1,800-stage group, twice per render). Fix: push into the existing array. Risk low.
20. **Non-memoized side panels recompute O(catalog+library) selectors on every 10–12.5 Hz poll tick.** `CastPanel.tsx:49-52`, `SceneBrief.tsx:33,89,153` (`wheelPool` not memoized), `AnchorPanel.tsx:38`; tick sources `controller.ts:419-431,442-448`, `reducer.ts:66-75`. Fix: memoize `wheelPool`; `memo` the panels with narrow comparators (template: `sameUnifiedBrowserInputs`). Risk low–medium.
21. **Keyboard/mouse listeners are torn down and re-registered 10×/s in ACTIVE mode** (`ui/src/input/useBrowserInput.ts:107` depends on `state.active`, a new array every 100 ms poll; handler reads only `.length === 1`). Fix: depend on `state.active?.length`. Risk low.
22. **`OnProjectPickables` looks up the same bones/bounds twice per candidate at 10 Hz; `VtableAddr(REL::ID(451614))` evaluated twice per poll.** `src/API/UIBridgeWorld.cpp:617-675` vs `:866-869`; `:521,536,562`. Fix: store the label point in `PickScreenBound`; `static const` vtable address. Risk low.

---

## Tier 2 — load / start / save-time cost (verified)

### Engine

1. **Clip bytes are read from BSResource (and copied again) before the import cache is consulted; every node transition re-reads every clip of the plan.** `src/Animation/GraphManagerClipLoad.cpp:164` (`ReadResourceFile`) → `:170/:179` (`ToByteVector`) → cache check only inside `GLTFImport::LoadAnimation` (`src/Serialization/GLTFImport.cpp:601-608`) / `AFImport.cpp:688-694`. `Scene.cpp:16-17` acknowledges it; `SceneRuntime_Graph.cpp:254` → `PrepareSynchronized` loads all stages per transition. Fix: `TryCached(cacheKey, animId)` before the read; have `ReadResourceFile` return `std::vector<std::byte>`. Risk low.
2. **Clip-library entries are installed-probed twice at load; each probe is three `current_path()` syscalls plus up to three BSResource opens.** `src/Registry/ContentRegistryClips.cpp:202-206` (private `installed` map) and `src/Registry/ContentRegistry.cpp:2480-2483` (tail loop over `next->scenes`, which now includes generated one-clip scenes not in `clipCache`); `src/Util/ClipPath.cpp:84,86,88` (`current_path()` ×3 per `ResolveClipSpec`). Vanilla packs register thousands. Fix: pass `ClipInstalledCache&` into `AddRegisteredClipEntries` and use the existing `ClipInstalled` helper (`:96-103`); cache `DataDir()` once. Risk low.
3. **`PublishProviders` deep-copies the entire base snapshot (every `SceneDef`, files, errors, availability) on every provider register/unregister and every `LoadAll`.** `src/Registry/ContentProviders.cpp:35`; callers `ContentRegistry.cpp:2488`, `src/API/EngineServices.cpp:110,206`. Normal boot pays base load + one full copy; each reload pays it again. Fix: layered snapshot (base + additions) or at least avoid the copy when the fresh snapshot is not yet shared. Risk medium (`RegistryRef::owner` pinning).
4. **Matchmaking re-derives actor sex and re-resolves keyword forms per def × role × permutation per query.** `src/Matchmaking/Matchmaker.cpp:137-141,215-250,114`. `AnchorMatchCache` (`Matchmaker.h:59-72`) already solves this for anchors. Fix: per-actor `{sex, race, keyword-hit cache}` in `BuildPool`; per-def n×n compatibility matrix before `FindRoleOrder`. Risk low.
5. **`MissingClipRefs()` re-probes every clip although the snapshot already carries `clipAvailability`** (`ContentRegistry.cpp:2617-2644` vs `:2478-2484`; Papyrus caller `OSFScript.cpp:690`). Fix: read `clipAvailability`, fall back on miss. Risk low.
6. **`Ba2::ReadFromArchive` byte-iterates the name table and allocates a lowercase string per entry per lookup; `ReadGameFile` scans every `.ba2`; `ClipDurationProbe::Probe` reads a whole `.af` for 64 header bytes.** `src/Util/Ba2.cpp:63,67-82,146-156`; `src/Serialization/ClipDurationProbe.cpp:219,232-234,242` (`cacheFresh` checked after the archive read). Driven per clip by the browser bulk probe. Fix: sized `read()`, allocation-free compare, per-archive memoized name index; 64-byte header read for `.af`. Risk low–medium.
7. **GLTF load copies the clip up to four times and parses the JSON twice.** `src/Serialization/GLTFImport.cpp:119,124/207` (`MaybeGunzip` twice), `:130,142` (nlohmann parse + dump), `:183-200` (`SanitizeGltfBytes` rebuilds the whole GLB incl. BIN), `:220` (`fastgltf::GltfDataBuffer::FromBytes` copies). Fix: skip sanitize when no `"min"/"max"` keys; splice only the JSON chunk; custom `GltfDataGetter`. Risk medium.
8. **`Graph::SetAnimation` rebuilds `jointMap`/`referencePose` on every stage switch even when the skeleton is the same object** (`src/Animation/Graph.cpp:91-114`, with `sameJointLayout` already known at `:53-59`), followed by a forced rebind that `ToLower`s every rig node (`:276`); on a job thread under two locks. Fix: build the lowercase map once on `OzzSkeleton` at import; skip `referencePose` when `skeleton == a_skeleton`. Risk low.
9. **`GLTFImport` lowercases node names repeatedly and uses `std::map` for the joint lookup** (`:263,380` `HasNodeNamed` twice; `:94,79,391,417,419`; `:382` vs `AFImport.cpp:546` unordered). **`AFImport::BuildAnimation` rotates each translation key three times; unfold vectors never `reserve`** (`AFImport.cpp:584-588`, `:417-442`). Risk low.
10. **`ActorState::Hide`/`ApplyPlan` re-read the actor's full inventory per acquired item** (`src/ActorState/Service.cpp:42-52` → `Store.cpp:122,155,196,213-219`; `ReadInventory` also sorts a canonical copy each time). ~O(N²) inventory walks under the engine inventory read lock per actor per start. Fix: `AcquireMany` reading once. Risk medium (`test/ActorStateTest.cpp` pins semantics).
11. **`PersistenceBroker::SaveToBytes` makes three copies of every client block per save and uses a bitwise (table-less) CRC32** (`src/Serialization/PersistenceBroker.cpp:369-387`, `:70-80`; CRC also on every load). Fix: serialize straight into `output` and patch the 20-byte header; 256-entry `constexpr` table. Risk low.
12. **`CatalogBuilder::Build` walks every node/stage/clip twice** (`src/API/CatalogBuilder.cpp:132-146`); runs per `ReadCatalog`, provider rebuild and reload. Fix: one pass. Risk low.
13. **`ClipDurations::KeyFor` round-trips through `std::filesystem::path`** (`src/Serialization/ClipDurations.cpp:21`; callers `CatalogBuilder.cpp:32,33,144`); **file-stat attribution re-keys by `path::string()` per scene/entry/problem** (`ContentRegistry.cpp:2364-2373,2457`, `ContentRegistryClips.cpp:138,283,297`); **known-size parser containers never `reserve`** (`ContentRegistry.cpp:648,841,872,1282,1373-1375,1397,1465-1483,1729-1734`). Risk low.
14. **`GetParticipants` copied by value just to test `.empty()`/`.size()`** (`src/API/EngineServices.cpp:141,254`; `OSFSceneAPI.cpp:295-306`). Fix: `ParticipantCount(handle)`. Risk low.

### Browser addon

15. **`BuildFileReport`/`BuildImportTextReport` deep-copy the file-stats vector** (`src/API/UIBridgeCatalog.cpp:396,484`: `const auto stats = …FileStats()` where `FileStats()` returns `const&`). Fix: `const auto&`. Risk low.
16. **`SendJson` copies each outbound document once more** (`UIBridge.cpp:120-121`: `const std::string text` passed to by-value `Send`, `BrowserJsonTransport.h:36`; ≈1.4 MB for `library.data`). Fix: `std::move`. Risk low.
17. **`BuildCatalog` sort comparator allocates two lowercase strings per comparison** (`UIBridgeCatalog.cpp:380-383`; ≈150k allocations per library build). Fix: `titleLower` member. Risk low.
18. **`ActorSpecies`/`ActorGender` each do a separate `ReadActorIdentity` JSON round trip; every caller wants both** (`EngineClient.cpp:39-44`; `UIBridgeWorld.cpp:711-712,1053-1054`, `UIBridge.cpp:934-935`). Fix: one `ActorIdentity(actor)`. Risk low.
19. **`ScanLabel` (a full catalog walk) runs twice per furniture pick because of eager debug-log arguments** (`UIBridgeWorld.cpp:743-745` then `:709`; same at `UIBridge.cpp:932,936,1116`). Fix: compute once into a local. Risk low.
20. **`ClipDurationCache::Lookup` allocates a `std::string` and takes the mutex once per stage during `Refresh`** (`ClipDurationCache.cpp:236-242`; `EngineClient.cpp:55-59`). Fix: snapshot once / heterogeneous lookup. Risk low.
21. **`stageHay` is computed for every library scene and never read** (`ui/src/model.ts:54-55,116`; `matchesSearch`'s library branch at `selectors.ts:376-388,403-411` is unreachable because active-mode candidates never contain library scenes). Fix: delete. Risk low.

### Tooling

22. **The full-repo source fingerprint (git ls-files + per-file `Get-FileHash` incl. 578 commonlib files + git status; 5.0 s cold / 1.5 s warm, measured) is recomputed ~11× per smoke run and 3× per packager.** `tools/SmokeBench.ps1:21-46`; callers `tools/test-smoke.ps1:111,141,253`, `tools/BuildBrowserAddon.ps1:12,67`, `packaging/ReleaseValidation.ps1:54,97`, `packaging/build-archive.ps1:52,127`, `packaging/build-browser-addon.ps1:26,93`. Fix: `-Identity` pass-through; keep the final recompute in `Write-ReleaseArchive`; one `SHA256.HashData` loop instead of the cmdlet per file. Risk medium.
23. **The smoke bench builds the browser DLL twice and runs the SDK-lock/boundary checks five times** (`test-smoke.ps1:137-138` builds all defaults, then `:167` → `BuildBrowserAddon.ps1:27-30` reconfigures and rebuilds; boundary at `test-smoke.ps1:131`, `test/SmokeBenchTest.ps1:19`, `BuildBrowserAddon.ps1:10`, `build-archive.ps1:53`, `build-browser-addon.ps1:27`; `Test-BrowserBoundary.ps1:33,36` reads every `src/**` file twice). Fix: `-SkipNative` for the bench. Risk medium.
24. **Skyrim importer decodes, hashes and (affine) retargets every clip twice per run** (`tools/skyrim-import/worker.py:146,160,175,189` vs `preflight.py:72-74,89-90`); **`retarget.py` recomputes the topological bone order on every frame, 2–3× per frame** (`retarget.py:58-73,76-79,254-286`). Fix: return decoded anims from `inspect_plan`; compute bone order once in `__init__`. Risk low.
25. **`generate_vanilla_packs.py` partially decompresses each `.af` three times and duplicates ~50 lines between human and creature classification** (`:139,144,196`; `:196-246` vs `:438-487`). Fix: one `classify_clip` reading one prefix. Risk low (packs are committed; diff to verify).
26. **`Run-Game.ps1` polls the Papyrus log directory recursively once per second for up to 150 s** (`test/loop-globals/Run-Game.ps1:231-238`). Fix: `Test-Path` on the fixed user-log path. Risk low.

---

## Tier 3 — simplification: duplication, structure, dead code (high confidence, low risk unless noted)

### Cross-cutting

- **Hand-rolled lowercase loops and case-insensitive compares beside `shared/Util/StringUtil.h::ToLower`.** Copies: `src/Audio/WwiseBackend.cpp:44-55` and `:129-140` (narrow + wide `LowerExtension`), `src/Serialization/AFImport.cpp:643-650` (`LowerKey`), `src/Matchmaking/RoleBinding.h:81-84` (lambda); char-equal lambdas at `RoleBinding.h:17-22`, `:47-50`, `extensions/browser/src/API/EngineClient.cpp:65`. Fix: add `Util::IEquals(string_view, string_view)` and `Util::ToLowerInPlace`; use everywhere (also unlocks the allocation-free fixes in tier 1 #7/#15).
- **`NowMs()` triplicated beside `Util::MonotonicNs`** (`src/Camera/CameraService.cpp:109-113`, `src/UI/FadeService.cpp:32-36`, `src/UI/Subtitle.cpp:44-48` vs `src/Util/MonotonicClock.h`); the atomic "extend deadline"/"expire in Tick" idioms duplicated between Fade and Subtitle (`FadeService.cpp:79-82,127-133` ≡ `Subtitle.cpp:111-114,119-126`). Fix: `Util::MonotonicMs()` + a tiny `Util::Deadline`.
- **The "major equal, minor ≤" ABI gate is hand-rolled four times** (`src/API/OSFSceneAPI.cpp:426-427`, `src/API/OSFEngineAPI.h:23-24`, `src/Serialization/PersistenceHost.cpp:540-541`, `src/API/OSFSettings.h:22`). Keep the public-header copies; one internal `PackedVersionCompatible` for the two exported request functions.
- **`ClientIDText` implemented twice with different output** (`PersistenceBroker.cpp:30-40`, `PersistenceHost.cpp:112-120`).

### Core playback (`src/Animation`, `src/Serialization`)

- **`Graph::rigIndexByJoint` is written in three places and never read** (`Graph.h:140`; `Graph.cpp:105,262,278`; repo-wide no reader). The write at `:277-279` is a second `jointMap.find` of the same key as `:288`. Delete.
- **`InvalidateBinding` and the `fail` lambda in `ResolveAndBind` are the same 11-line reset** (`Graph.cpp:119-135` vs `:178-199`; partial third copy `:320-322`). `fail` = `InvalidateBinding(true)` + counter/trace. Extract `ResetBaseCapture()`.
- **The stage-apply sequence is written twice and placement composition three times** (`Graph.cpp:405-414` vs `GraphManager.cpp:537-556`; `SetAlignmentDelta :984`; `kNoPreservedBones` declared twice `:337,:539`). Fix: `Graph::ApplyStage(SegmentData, stageIdx)`; `SetPreserveBones(std::span)`. Risk low–medium (the two sites order policy/animation differently; both work because the rebind happens later).
- **Fourteen `GraphManager` accessors repeat the lock/find/lock/validate preamble with inconsistent lock release order** (`GraphManager.cpp:352,631,667,677,689,706,726,863,882,896,942,963,976,993,1093`). Fix: `WithGraph(actor, fn)` + `OwnedSessionLocked(graph, expectedId, sink)`; ~60 lines removed.
- **`GraphManagerClipLoad::Load` duplicates the af/gltf import+error block four times and the success tail twice** (`:136-154`, `:169-187`, tails `:155-161/:188-194`). Resolve bytes + key first, one branch. Pairs with tier 2 #1.
- **"Find the `shared_ptr` for a raw `PlaybackSession*`" written three times** (`GraphManager.cpp:1237-1238,1314-1326,1335-1340`). `FindSessionLocked`.
- **`SetSpeed` takes, releases and re-takes manager+graph locks for the no-sink case** (`:941-968`). Write `session->speed` in the first critical section when `sinkId == 0`.
- **Plan shape validated three times** (`Scene.h:195-206` `HasValidRolePolicyShape` (one caller), `GraphManager.cpp:168-173`, `Scene.cpp:20-21`).
- **Dead aliases in `Scene.h`:** `:72-73` (`kTimer`, `kLoops`), `:143` (`Stage`), `:236` (`StageData`).

### Scene runtime and registries (`src/Scene`, `src/Registry`, `src/Matchmaking`, `src/Preparation`)

- **`ContentRegistry.cpp` hand-rolls JSON field checks that `src/Util/JsonField.h::JsonView` already provides** — 8 `JsonView` uses vs 104 manual `is_*()` checks and 46 unchecked `.value()` reads. Concrete pairs: `weight` `:1219` (helper) vs `:1931-1938` (manual); booleans `:1895-1899,1911-1913` vs `:1221-1229`; `tags` `:298-300` (`ReadList`) vs `:1939-1951,1324-1334`, unchecked `:1665-1669,1737-1738` (wrong type leaks nlohmann's `type_error.302` text); strings `:1409-1414,901-906,933-948,189-194,1318-1323`; unchecked typed reads `:650-659,331-340,1424,1458-1459,1294-1295,874,885,112-115`. Missing helper: a float reader. Migrate function by function; check `test/ContentRegistryTest.cpp` message asserts.
- **`ParseRole` rebuilds its subject prefix 14×; `ParseTrackTiming` 9×** (`:895-1022`, `:397-452`); a `JsonView` with exactly that subject already exists at `:913`.
- **`offset` object guard + `ParseOffsetField` repeated 3× (`:195-199,992-997,1630-1634`); `mask` parse 2× (`:201-211` vs `:962-974`).**
- **Prop attachment-node validation duplicated** (`:564-570` vs `:532-540`).
- **`ExpandSoundLadder::emit` serializes a throwaway `json` just to re-parse it with `ParseTrackTiming`** (`:743-746`), forcing duplicate `at`/`atFrame` conflict checks at `:750-752,818-821`. Split `ParseTrackPosition` + `ParseRepeat`.
- **File-local `ResolveFormRef<T>` duplicates `Util::ResolveFormRef<T>`; anchor block hand-writes the messages a third way** (`:65-80` vs `src/Util/FormRef.h:80-85`; `:1606-1626`).
- **Over-long parsers with natural seams; one block runs for nothing.** `ParseOsfScene :1640-1818` (179 lines, already sectioned), `ParseOsfFileDefaults :1852-1979`, `LoadAll :2356-2492`, `ParseRole`, `ParseOsfStageList :1275-1390`, `ExpandSoundLadder`, `ParseOsfNode`. In `ParseOsfStageList`, when `!a_allowStageLanes` the code throws at `:1336-1341` on any lane key and then still builds `SceneNode scratch` and does four `find`s that can only miss (`:1342-1361`) — wrap in `if (a_allowStageLanes)`.
- **`hasPlayer` preamble duplicated seven times** (`SceneRuntime_Graph.cpp:119,270,287,326,380`; `SceneRuntime_Dispatch.cpp:221,293`). `HasPlayer(span)` in `SceneFoundation.h`.
- **"Push ledger entry if absent" open-coded five times beside `RecordMechanism`** (`SceneRuntime_Ledger.cpp:209-211,236-239,262-264`, `SceneRuntime_Props.cpp:93-97`, `SceneRuntime_Preparation.cpp:453-455`; canonical `Ledger.cpp:25-28`). `PushLedger(Slot&, Mechanism)`.
- **`CompleteStart` takes two parameters it discards** (`SceneRuntime_Graph.cpp:340-349`; sole caller `SceneRuntime_Preparation.cpp:463`; decl `SceneRuntime.h:425-427`).
- **`PreparedTransition::defaults` always equals `kind == Start || kind == Handoff`** (set `Graph.cpp:490,656`, `Handoff.cpp:73`; read once `Preparation.cpp:316`).
- **Three one-caller `StartFromDef*` wrappers each `Find` a def that `Start` immediately `Find`s again** (`SceneRuntime_Graph.cpp:588-609,667-689,443-447`; callers `SceneLauncher.cpp:152-156`). One `StartFromDef(…)` passing the `SceneRef` through.
- **`ApplyTransition` re-snapshots the slot, so the `sceneId`/`participants` its five callers copy under `_lock` are only forwarded** (`:395-399`; callers `:98-113,504-527,694-733,783-847,532-560`). Risk medium (relies on the `transitioning` invariant).
- **API edge query rebuilds `AdvanceEdges` 2n+1 times** (`src/API/EngineServices.cpp:269-270` → `SceneRuntime_Graph.cpp:873-888` each → `:851-871`). Make `AdvanceEdges` public.
- **`_lock` held across file I/O, the alignment store mutex and engine calls** (`SceneRuntime_Alignment.cpp:41-48` disk write under `_lock`; `:17-40`; `SceneRuntime_Handoff.cpp:19-56` holds `_lock` across `Find`, `BindSceneRoles` engine calls, `RenderedTransform`'s `loadedData.LockRead()`; `SceneRuntime_Preparation.cpp:84-97` JSON dump under lock). `AlignmentInput` (`:50-93`) already shows the copy-act-recheck shape. Risk medium (locking).
- **`PersistenceHost::BeginLoad` holds `_lifecycleMutex` across client `revert` callbacks** (`PersistenceHost.cpp:200-216` → `PersistenceBroker.cpp:419-423`); any re-entry self-deadlocks. Scope the lock to the flag/table updates. Risk low (no deadlock observed).
- **Dead:** `Coordinator::Reset` (`src/Preparation/Coordinator.cpp:114-123`, `.h:57`), `AnchorAccepts`'s `a_matchedKeyword` out-param (`Matchmaker.h:57`, `.cpp:261-287`; both callers pass two args), `SoundRegistry::Size/LoadErrors` (`.cpp:378-388`, `.h:60,62`), `TrackFraction/TrackFires` (`ContentRegistry.h:176-197`; test-only).

### API, Papyrus, services (`src/API`, `src/Papyrus`, `src/Camera`, `src/Player`, `src/Audio`, `src/UI`)

- **The one-stage "files" plan and the clip-spec split loop are built three times** (`OSFScript.cpp:468-479,526-549,625-638`; `OSFSceneAPI.cpp:186-209`). `Scene::MakeFilesSegment` + `StartFilesHold` in `SceneLauncher`.
- **The native API re-implements `SceneLauncher`'s `MakeOverrides/MakeAnchor/MakeDefLaunchOptions`; `anchorBase` resolved 2–3× per start** (`OSFSceneAPI.cpp:38-52,54-65,84-98` vs `SceneLauncher.cpp:64-75,118-127`; redundant `:148`; `StartSceneFiles :208` calls `MakeAnchorAt` → `RenderedTransform` twice). Only `invalidStage`/`emitHud` differ (documented adapter choices). Risk medium (verify `headingRad` plumbing).
- **`StartSceneByTags` / `StartSceneByTagsQuery` / `StartSceneAtAnchor` share one body; `TagQuery` construction repeated 5×, `AnchorMode` 3×** (`OSFScript.cpp:349-364,397-413`; `:323,334,361,377,391`; `:335,378,394`). `ReplaceSceneByTagsQuery :320-324` ≡ `ReplaceSceneByQuery(…, {}, nullptr, false)`.
- **`StopSceneForActor` duplicated verbatim in Papyrus and native API; `StopAllForActors` re-implements it in a loop** (`OSFScript.cpp:260-270` ≡ `OSFSceneAPI.cpp:216-225`; `:650-676`). One `StopAnyPlaybackForActor`.
- **C-string-array → `vector<string>` hand-rolled 3× with drifted caps/null policy** (`EngineServices.cpp:238-248,326-331`, `OSFSceneAPI.cpp:178-182`; `MatchCast` re-implements `CollectActors` `OSFSceneAPI.cpp:67-82` vs `EngineServices.cpp:50-51`).
- **34 identical `try { … } catch (...) { return fallback; }` ABI wrappers** (`EngineServices.cpp` ×18, `ActorStateAPI.cpp` ×9, `PreparationAPI.cpp` ×7). A `NoThrow<Fallback>(f)` helper; `ActorStateAPI` already has `Result()` for half.
- **`ActorEquipmentRequest` validation duplicated with a drifted flag mask** (`ActorStateAPI.cpp:29-31` `~3u` vs `PreparationAPI.cpp:61-62` `~ActorEquipmentAllowAdd`).
- **`CameraService::suppressBounce` is a lock-free mirror of `overrideCount > 0`** (`CameraService.cpp:347-350,754-762,927-930`); make `overrideCount` atomic, delete the flag. **Three small sequences repeated across queued tasks:** stop-orbit+release-mouse `:473,509,773,912`; ensure-third-person `:243,271,547,787` (+`:485,523`); queued-task preamble `:376,477,504,568,585,826`. `StopOrbitDrive()`, `EnsureThirdPerson()`, `QueueOverrideTask()`.
- **`PlayerInputLockService`: `ApplyDisabled`/`RestoreEnabled` are one function with a bool; `userMask`/`otherMask` are constants copied by a constructor; `SetStandaloneLock` has no caller** (`.cpp:153-177,48-51`; `.h:43-44,25`).
- **Dead public entry points:** `HudMessage::Debug/DebugEnabled` (`src/UI/HudMessage.cpp:44-54`, `.h:14,17`; `g_debugEnabled` then has no reader), `Wwise::PostExternalFile` (`WwiseBackend.cpp:408-411`, `.h:34`), `Wwise::HashEventName` public but internal-only; `PersistenceBroker` `AtomicReplacer` injection point never used (`.h:23,61-67`; `.cpp:573-577,603`) and `LogLevel::kDebug/kInfo` never emitted.
- **`InputService` and `WeaponService` are member-less "singleton" classes over file-scope globals** (`InputService.h:22-57` vs `.cpp:24-45`; `WeaponService.h:5-15`). Convert to namespaces like `UI::Subtitle`/`UI::HudMessage`. Cosmetic.

### Browser addon (`extensions/browser/`)

- **JSON helpers (`ParsePayload`, `Int32Value`, `NumOr`, `IntOr`, `StrOr`, `SendJson`) duplicated verbatim between `UIBridge.cpp:115-270` and `UIBridgeWorld.cpp:72-152`; `UIBridge.cpp`'s `StrOr` has no caller; the two `SendJson`s differ (one chunks, one emits directly).** One `UIBridgeJson.h`, one transport path.
- **Dead UI values:** `"library"` mode (`state.ts:79`, `controller.ts:485`, `settings.ts:61` — `decodePreferences` restricts `openTo`), `"action"` browse kind (`state.ts:82-87`), `WheelEntry.detail` (`state.ts:144`, `controller.ts:264`; native emits only `scene/title/stage`).
- **A second `OSFUI::API::Client` and three copies of `kViewId`** (`UISettings.cpp:23-25,153,158` vs `BrowserSession.cpp:16,65`; `BrowserSession.h:8`, `UIBridge.cpp:104`, `UISettings.cpp:23`).
- **`OpenBrowser`/`OpenWheel` duplicate the menu-closing block** (`UIBridge.cpp:1118-1127` ≡ `:1166-1175`). **Relative-pointer trace globals exist only for debug logging on the per-input path** (`:69-73,1049-1080`). **`WheelPins::Entry` JSON shape hand-rolled in three places** (`WheelPins.cpp:63-83,93-99`, `UIBridge.cpp:818-846`, `UIBridgeCatalog.cpp:234-243`). **`ActorCountOf` is a pass-through with a stale comment and an unused `using`** (`UIBridgeCatalog.cpp:163-167`, `.h:18`, `UIBridge.cpp:46`).
- **Mirror-image reducer cases** (`reducer.ts:108-111,346-349,165-188,244-270`; `controller.ts:598-599` `cancelWheel` ≡ `requestClose`). **`ui/vitest.config.ts` re-registers `octane()` (a `test:` key in `vite.config.ts` removes it); `package.json:12` `tsc --noEmit` while tsconfig already sets `noEmit`; `views/osf.animation/browser/main.tsx` is a one-line shim (`build.rollupOptions.input` avoids it).** **`osfui.d.ts` exports a dozen types nothing imports** (`:7-19,54,56-103,106-123`; only `JsonObject`/`OSFUIBridge` used). **`dev/mock-bridge.ts` drift** (`:153` `detail/key`, `:217` failure shape omits `sceneId`/`handle`, `:86` no `relativePointer`).

### Build, tooling, tests, Papyrus

- **`xmake.lua` test targets repeat 4–6 boilerplate lines each; 10 source files are compiled 2–3× across targets** (`xmake.lua:82-227`; `Gzip.cpp` ×3, `ClipPath.cpp` ×3, `Coordinator.cpp` ×3, `GLTFImport/AFImport/Ba2/Scene/ClipDurations/NativeMainThreadQueue/ClipDurationCache` ×2). Fix: an `osf_test(name, opts)` Lua helper; static libs `osf-gltf-core` (`GLTFImport+Gzip`) and `osf-af-core` (`AFImport+Ba2`). **Caution:** `tools/SmokeBench.ps1:250-255` and `tools/Test-BrowserBoundary.ps1:19-29` discover targets by regex over literal `target("…")` blocks containing `add_tests(` — update the regex in the same change or the bench inventory goes empty.
- **The CK Papyrus compiler invocation is copied in five places** (`.vscode/tasks.json:50-59`, `packaging/build-archive.ps1:39-46`, `tools/BuildSmokeFixtures.ps1:54-62` ×2, `test/loop-globals/Run-Game.ps1:198-199`, prose `AGENTS.md:37-43`). One `tools/Invoke-PapyrusCompiler.ps1`.
- **`build-archive.ps1` and `build-browser-addon.ps1` re-implement the same staging steps** (version parse ×4 with two regex variants `build-archive.ps1:25-27,54`, `build-browser-addon.ps1:16-18,28`; FOMOD version stamp `:90-93` vs `:72-76`; license copy `:72-78` vs `:45-53` with different missing-file policy; folder check `:116-120` vs `:78-84`). Move into `ReleaseValidation.ps1`.
- **Two "production build + receipt" implementations with different configure flags** (`tools/BuildProduction.ps1:11-22` vs `tools/test-smoke.ps1:135-141`; identical prologue `BuildProduction.ps1:7-26` vs `BuildBrowserAddon.ps1:8-35`). Have the bench call `BuildProduction.ps1`. Risk low–medium (flags must match the receipt's `configuration` string).
- **Two git-process runners** (`SmokeBench.ps1:4-19` vs `Test-SdkLock.ps1:29-46`).
- **`BuildSmokeFixtures.ps1` and `BuildD3Fixtures.ps1` duplicate the glTF head-rotation doc, the WAV writer and the build-root guard** (`:14-27/42-50/7-9` vs `:12-26/27-42/6-8`); `BuildD3Fixtures.ps1` is referenced nowhere in the repo (may belong to the sibling harness — confirm before deleting).
- **Native test suites duplicate `TempDirectory`, `Write`, `Near` (four tolerances, two signatures) and `Run`** (`test/AFFixtureTest.cpp:56-90,212,302-313`, `test/ImportSmokeTest.cpp:33-80,184,255-266`, `test/AdditivePoseTest.cpp:23`, `test/PlaybackSessionTest.cpp:60-74`; ad-hoc temp dirs `BrowserBoundaryTest.cpp:312-360`, `ClipDurationTest.cpp:164,253`, `PersistenceTest.cpp:392-405`). Extend `test/Check.h`.
- **Three hand-rolled mutex/condvar "gate" latches in tests** (`test/NativeSceneEventTest.cpp:61-76`, `test/ClipDurationTest.cpp:25-46`, `test/SoundServiceTest.cpp:32-55`). Promote `ClipDurationTest`'s `Gate`.
- **The two PowerShell contract tests duplicate their scaffolding** (`test/SmokeBenchTest.ps1:8-16,324-328` vs `test/ReleaseValidationTest.ps1:8-18,124-128`).
- **`preview_audit._clip` re-parses the GLB that `validate_glb` just parsed with a second, less-checked accessor reader** (`tools/skyrim-import/preview_audit.py:20-49` vs `target_rig.py:195-226`).
- **`OSFTest.psc` has two near-identical 8-line bodies** (`dist/Scripts/Source/OSFTest.psc:79-87` vs `:94-102`); the forwarders at `:57-59,75-77,90-92` are justified by the comment at `:35` and must stay.

---

## Tier 4 — stale files and tracked machine state

- **`experiments/af-idle/**`** (26 tracked files incl. `OSF.esm`, four `.pex` force-added past `.gitignore`, 4×`.af`, a 2074-line `branch.patch`): nothing in `src/`, `extensions/`, `tools/`, `packaging/`, `xmake.lua` or docs references it; it does not duplicate live code. Its only cost is being hashed into every source fingerprint (tier 2 #22). Option: keep as a tag, retain `README.md` + `manifest.json`.
- **`tools/profiling/Capture-Cpu.ps1:64,67`** references a `build\profile` output dir and an "OSF Animation Profiling" mod that no script or target produces (filtered by `Test-Path`, so harmless but misleading).
- **Machine-specific paths tracked:** `.claude/launch.json:13` (sibling `OSF UI/frontend`), `.vscode/c_cpp_properties.json:11` (explicit MSVC 14.51 path, un-ignored by `.gitignore`), `test/loop-globals/Run-Game.ps1:4,11-12` (absolute sibling/MO2 paths; `test-smoke.ps1:17` already derives the harness path relative to the repo).
- **`tools/alignment/`** contains only `__pycache__/` (untracked, ignored); the module that produced it is gone.
- **`osf-af-import-test`** (`xmake.lua:103-108`) is never run by any script — by design (manual CLI needing real `.af`/`.rig`); `osf-af-fixture-test` covers it. Note only.

---

## Checked and found clean (so nobody re-reviews them)

- `StampPose` is allocation-free in steady state; row layout respected throughout `PoseMath`; `captureLiveBase` refuses to grow buffers in the stamp hook.
- Both animation hooks are lock-free when idle; the `deferred` vector never allocates unless used; manager lock released before `FlushDeferredTasks`; lock order manager → graph → session/syncGroup is consistent; sinks never invoked under manager/graph locks.
- `MaintenanceTick` early-outs, rate-limits the stall scan (250 ms), collects under shared lock and removes outside it. `NativeMainThreadQueue::Drain` swaps out under lock, runs unlocked, thread-local re-entrancy guard.
- `CameraService::Tick`, `Subtitle::Tick`: lock-free, allocation-free, REL IDs resolved once into statics.
- Import caches never hold their mutex while parsing; AF decode and BA2 reader are bounds-safe with capped allocations; `PreparePlaybackStages` dedupes identical clip specs within a preparation.
- `SceneDef::tagSet` is prebuilt lowercase (allocation-free `TagsMatch`); `AnchorMatchCache` memoizes keyword hits; registry discovery is one filesystem walk per suffix; no `std::regex` anywhere in `src/` or `extensions/`.
- `RecordMechanism`/`UndoMechanism`/`ReplayLedger`/`AttachSceneProp` copy under `_lock` and call services outside it.
- All 47 `OSF.psc` + 11 `OSFAdvanced.psc` natives are declared and bound 1:1 (`OSFScript.cpp:711-774`); `tools/verify-papyrus.py` enforces it at packaging. No pure forwarding wrappers in `OSF.psc`. `OSFTest.psc` is dev-only and rejected by both archive checks.
- Every `#define`/option is read (`OSF_ANIMATION_SMOKE`, `OSF_AUDIO_TEST`, `animation_smoke`). `.gitignore` covers build outputs; `sdk-lock.json`/FOMOD/versions (1.5.0) consistent.
- Browser: `BrowserSessionQueue`/`BrowserSession` generation+visibility design, `BrowserLaunchSequence` flags, `BrowserJsonTransport` chunk pacing/dedupe/epoch cancel, `ClipDurationCache` lock discipline, `WheelPins` atomic replace, `OnScanNearby` inverted keyword index, the `playableItems`/`wheelCandidates`/`castTokens` memos and `UnifiedBrowser`'s comparator are all correct; `actions.ts`/`commands.ts` have no dead members.
- `src/Testing/*` has no large dead or duplicated blocks; `CrashHandler` correctly bypasses spdlog.
