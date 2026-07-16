# OSF Gameplay Modules: Ambient NPC Life Director + Player Immersion Actions

## Context

OSF Animation is a content-neutral animation/scene framework for Starfield, but its only out-of-box user experience is the Scene Browser — an isolated sandbox that doesn't touch playthroughs. The goal is to ship gameplay value out of the box while preserving the framework's mechanism/policy split (policy lives in consumers, per AGENTS.md). Two features were chosen, both packaged as **optional first-party modules** (FOMOD-selectable) that consume OSF's own public surfaces — dogfooding the API:

1. **Ambient NPC Life Director** — a background service that opportunistically casts idle, eligible NPCs into tagged ambient scenes (sitting, leaning, console work, eating, paired chats) built entirely from vanilla `.af` clips. Passive immersion in every city/outpost/ship.
2. **Player Immersion Actions** — sit/lean anywhere via hotkey, an emote wheel (with paired NPC responses via crosshair target + matchmaking), and cosmetic item-use animations (eat/drink/med).

Key enabler verified in repo: the framework already plays the full vanilla clip library (`dist/OSF/vanilla/` contains generated packs with resolved anchor keywords, clip paths, and probed durations) — **zero new animation assets needed**. Free scenes co-locate at actor[0] (`src/API/OSFSceneAPI.h`), the undo ledger restores control/equipment/weapons/camera on any termination, and player lock/camera/fade only engage when the player participates — so NPC-only ambient scenes are safe by construction.

**Module gating pattern (both features):** no `settings.json` feature toggle (AGENTS.md rule). Each module is enabled by the *presence of its FOMOD-installed content folder* — `Data/OSF/AmbientLife/ambient.json` for the director; the immersion pack + `OSFImmersion.esm` for immersion actions. Absent folder → zero runtime cost.

---

## Phase 0 — De-risking spikes (in-game, minimal/no code)

Do these before building; each has an existing console-test path (`OSFTest.psc` via `cgf`).

1. **NPC-only scene on a sandboxing NPC** (`cgf "OSFTest.Furniture2"` / `PairId` on settlement NPCs): AI package doesn't fight the scene; NPC resumes AI cleanly on stop; ledger restores gear.
2. **Furniture occupancy**: hand-author one throwaway ambient scene on a chair anchor (copy anchor keyword + clip + `sec` from `dist/OSF/vanilla/vanilla-furniture.osf.json`); start it on occupied furniture. Decide the occupancy rule (proposal: reject anchors with another high-list actor within ~64 units).
3. **`stripActors:false` + no player-side effects** for NPC-only scenes (belt-and-braces check of `SceneRuntime` player-participant gating).
4. **Item-consumed event (highest immersion risk)**: dev-only Papyrus spike registering `RegisterForRemoteEvent(player, "OnItemEquipped")` (pattern: `OSFDataSlateManager.psc`) — does it fire on food/drink/med consumption from inventory AND favorites? In parallel, a log-only native `BSTEventSink<RE::TESEquipEvent>` spike (`lib/commonlibsf/.../Events.h:3823`; sink pattern: `src/Serialization/SaveSafety.cpp`). Decision gate: Papyrus works → module stays pure Papyrus; only native works → add a content-neutral "player consumed item" relay event (no keyword→tag mapping in the DLL). Identify vanilla food/drink/med keywords while testing.
5. **Clip suitability** (`cgf "OSFTest.Solo"`): `sitonground_pose.af`, `walllean*` enter/idle/exit chains free-standing, `eat_stand_start.af`, `wave_pose.af`, photomode idles — loopable vs one-shot, and whether furniture-family clips look right free-standing (tune per-role `offset` if not).
6. **View mode-switch timing**: `RequestMenu("osf", true)` then `SendToWeb("osf", "osf.mode", ...)` reaches the view after visibility; view receives JS key events while overlay focused.

Exit criteria: safe anchor-keyword families list, occupancy rule, item-event winner, curated clip shortlist.

---

## Phase 1 — Shared framework foundations

Refactors + small mechanism additions used by both modules (and any third-party consumer):

1. **Extract `EnumerateHighActors`** from anon namespace in `src/API/UIBridge.cpp:644-672` → new `src/Util/ActorScan.{h,cpp}`; UIBridge adopts it. Preserves the "high list only, vtable-check" safety comments.
2. **Extract the matchmake-and-start funnel** (`StartMatched` + `ResolveSceneAnchor` + `StartCandidate`, `src/Papyrus/OSFScript.cpp` ~480-560) → new `src/Scene/SceneLauncher.{h,cpp}` (`LaunchMatched(actors, tagQuery, anchor, anchorMode, overrides, logTag)`); Papyrus natives become thin wrappers. One funnel, three consumers (Papyrus, director, hotkey exec).
3. **Extract anchor search near a point** from `OnScanNearby`'s furniture branch (UIBridge.cpp:734-825) → `src/Scene/AnchorSearch.{h,cpp}`: `FindAnchorsNear(origin, radius, defFilter) -> vector<AnchorHit>`.
4. **Global hotkey channel**: `src/Input/InputService.{h,cpp}` gains an always-on hotkey table (press-edge in `Thunk` before grant-gated block; gate on `!MenuOwnsInput()` and overlay-closed; dispatch via `SFSE::GetTaskInterface()->AddTask`, mirroring `MaybeDispatch`). New `src/Input/HotkeyService.{h,cpp}` executes command strings: `openBrowser`, `openWheel[:tagPrefix]`, `toggleSceneTags:<tags>` (live tagged scene → `Advance` to its exit node, else start via `SceneLauncher`).
5. **`settings.json` `"hotkeys"` map** in `src/Config/Settings.{h,cpp}` (e.g. `{"F10":"openBrowser","G":"openWheel","N":"toggleSceneTags:player.sit"}`; VK-name map, bad entries → `[Config]` ERROR + skip). Wire in `src/main.cpp` at `kPostPostDataLoad` next to `InputService::Install()`.
6. **Missing `GetSceneId(handle)` Papyrus native** — promised in `OSF.psc:12` comments but never bound in `src/Papyrus/OSFScript.cpp`. Add it (+ optionally `GetSceneTags`); recompile `.pex` per AGENTS.md.

---

## Phase 2 — Ambient NPC Life Director (native, in-DLL)

**Architecture:** native `src/Director/AmbientDirector.{h,cpp}` singleton compiled into the DLL, log tag `[Ambient]`. Rejected alternatives: Papyrus quest (no VM-side high-actor enumeration; scan cadence = VM thrash) and second DLL (native API lacks tag matchmaking/end callbacks today). Precedent: UIBridge is already an in-DLL first-party consumer. Enabled iff `Data/OSF/AmbientLife/ambient.json` exists (FOMOD-installed).

- **Config** `src/Director/AmbientConfig.h`, loaded from `ambient.json`: `scanIntervalSec` (12), `watchIntervalSec` (1), `maxConcurrent` (3), `maxStartsPerScan` (1), per-NPC/per-anchor cooldowns, `radius` (4096), `playerMinDistance` (350), `anchorSearchRadius` (700), `maxSceneSec` (120), `pairChance`, `tags` query, `skipEssential/Unique/Teammates`.
- **Scheduling:** one `AmbientDirector::Tick()` call added to the per-frame heartbeat in `GraphManager::Hook_AnimGraphUpdate` (`src/Animation/GraphManager.cpp:1364-1367`, next to CameraService/FadeService/StallWatch ticks — verified). Tick is an atomic throttle; due work posts to game thread via `SFSE::GetTaskInterface()->AddTask` (pattern at GraphManager.cpp:1343).
- **Candidate scan:** skip if player in combat/in scene/at cap. `Util::EnumerateHighActors` → filter: alive, distance band, `!IsInCombat()`, not essential/unique/teammate (config), `Util::ActorSpecies == "human"`, not in a scene (`GetSceneForActor==0`), not on cooldown. Prefer NPCs away from the player (teleport-snap is visible up close). Start order per candidate: furniture-anchored (`AnchorSearch` + occupancy rule + anchor cooldown) → pair chat (`pairChance`, second candidate within ~512u; matchmaker role-count auto-selects 2-role scenes) → free-standing solo. `StartOverrides{strip=false}`. Track `{handle, actors, startTime, anchorRef}`; stamp cooldowns.
- **Watch tick (1 s):** dead handle → drop bookkeeping (self-heals across save/load via existing SaveSafety teardown); participant in combat/dead → stop; participant beyond `radius*1.25` → stop; `> maxSceneSec` → stop. Ledger handles all restoration.
- **Event sinks** (register at `kPostDataLoad`, pattern `SaveSafety.cpp:61-150`; degrade to WARN on failure since StallWatch + ledger already backstop): `RE::TESActivateEvent` (player activates a cast NPC → stop immediately; covers dialogue), `RE::CellAttachDetachEvent` (detach → stop matching handles). Combat stays polled — CommonLibSF has no `TESCombatEvent` (verified in `Events.h`).
- **Wiring:** `src/main.cpp` `kPostDataLoad` after `SceneRegistry::LoadAll` → `AmbientDirector::Install()`; `[Feature] Ambient Director ENABLED (N 'ambient' scenes)` / `DISABLED` report line. Handle `ReloadPacks`.
- **Debug natives** on `OSFAdvanced.psc`: `SetAmbientEnabled(bool)`, `AmbientScanNow()`, `AmbientReport()` — director core is testable via `cgf "OSFAdvanced.AmbientScanNow"` before sinks exist.

**Content pack** `dist/OSF/AmbientLife/` (deployed to `Data/OSF/AmbientLife/`):
- `ambient.json` (config = enable switch)
- `ambient-furniture.osf.json` — ~8 anchored solos curated from `vanilla-furniture.osf.json` (copy clip paths/anchor keywords/`sec`; don't `use`-reference multi-stage library scenes): sit-barstool/bench/chair, lean-desk, console-stand/sit, eat/drink. File-level `"stripActors": false, "lockPlayer": false`.
- `ambient-standing.osf.json` — ~4 free solos + ~2 pair-chat scenes (2 anonymous roles with facing offsets `{y:100, heading:180}`-style — the required mitigation for the clips-stack-at-anchor caveat).
- All scenes self-terminating (enter → idle `loops:3-6`/`timer:30-60` → exit); tags `["ambient", "ambient.<category>"]`; gender-only role filters in v1 (form-ref resolution is flagged RE-sensitive). Document the `ambient` tag contract so third-party packs can join.

---

## Phase 3 — Player Immersion Actions

**Split:** hotkeys/wheel-UI/target-capture = framework mechanism (native + `views/osf`); scenes = content pack `dist/OSF/immersion/`; item-use policy = small module ESM + Papyrus (`OSFImmersion.esm` + `OSFImmersion.psc`, Spriggit pattern from `Plugin/` + `Build-Plugin.ps1`).

1. **Sit/lean-anywhere** — `dist/OSF/immersion/sit-lean.osf.json`, pure content (free scenes co-locate at the player; director-channel Space=Advance/End=End already granted by default, so graph scenes get "press Space to stand" for free): `immersion.sit.ground` (graph: enter → idle `loops:0` → advance-edge → exit → `$end`), `immersion.lean.wall.*` (walllean enter/idle/exit chains; player lines up with the wall in v1 — raycast snap noted as future work), kneel/ledge variants per Phase 0 findings. Tags `player.sit`/`player.lean`; `stripActors:false`; `camera: thirdperson_hold`; exit via Space, End, or hotkey re-press (`toggleSceneTags:player.sit`).
2. **Emote wheel** — no second Ultralight view needed (view membership lives in OSF UI's config): `HotkeyService` `openWheel` → new `UIBridge` `OpenWheel()` captures `player->commandTarget` (reuse `OnPickCrosshair` validation + token alloc), `RequestMenu("osf", true)`, `SendToWeb("osf","osf.mode",{mode:"wheel", tagPrefix:"player.emote.", targetToken/name/valid})`. View (`views/osf/main.js` `onNativeMessage` dispatch ~line 126, + `index.html`/`style.css`): radial overlay from already-loaded catalog filtered by tag prefix; pick → new `osf.wheel.pick {sceneId, targetToken?}` → launch via existing `OnLaunch` machinery, refuse hostile/in-combat/dead targets, then `RequestMenu("osf", false)`.
3. **Paired emotes (v1, no teleport):** valid target → launch TWO free scenes: `player.emote.<name>` on the player, matchmade `npc.emote.response.<name>` on the target (each co-locates at its own actor; role filters gate eligibility). Synced two-role handshake/hug scenes are a later content-only addition.
4. **Item-use animations** (per Phase 0 winner; assume Papyrus): `OSFImmersion.psc` quest re-arms `RegisterForRemoteEvent(player,"OnItemEquipped")` on init/load; on ALCH fire → keyword classify (Properties filled in ESM) → guards (not in OSF scene, not in combat, weapon sheathed, ~5 s cooldown) → `OSF.StartSceneByTagsQuery(["player.item","player.item.<cat>"])`. Content `item-use.osf.json`: food/drink/med scenes ≤5 s, `stripActors:false`. Animation fires *after* the engine consumes — cosmetic only, no double-consume risk by construction.
5. **Emote content** `emotes.osf.json`: wave/whatsup/thumbsup/cheer/dance/handsonhips/kneel from `dist/OSF/vanilla/common/` + photomode idles; short (`loops:2`/`timer:4`), weapon-sheathe action on enter; matching `npc.emote.response.*` scenes with human-only role filters.

---

## Phase 4 — Packaging, docs

- `packaging/fomod/ModuleConfig.xml`: two new SelectAny groups — "Ambient NPC Life" (installs `AmbientLife/`) and "Immersion Actions" (installs `immersion/` pack + `OSFImmersion.esm` + `.pex`), mirroring the SafCompat pattern; `packaging/build-archive.ps1` stages both.
- Core gains default `hotkeys` in `dist/settings.release.json` (browser hotkey `F10` is a nice standalone win — today the browser only opens via the Data Slate book).
- Docs: AGENTS.md (`[Ambient]` log tag; first-party-modules architecture note), `docs/API.md` (`GetSceneId`), `Settings.h` schema comment (`hotkeys`), `docs/GETTING_STARTED.md` (director as second worked consumer example next to SIF), tag-contract doc (`ambient`, `player.*`, `npc.emote.response.*`), CHANGELOG.

## Verification

- Console harness: `cgf "OSFAdvanced.AmbientScanNow"` / `AmbientReport`; dev-only `OSFImmersionTest.psc` (`.Sit`, `.Emote "wave"`, `.PairedWave <npc>`, `.EatTest`), excluded from release like OSFTest.
- Ambient in-game checklist: New Atlantis + outpost + ship — scenes within ~1 min, ≤ maxConcurrent, no essential/unique/follower cast; activate a cast NPC → scene stops, dialogue opens, NPC dressed/armed; combat nearby → stop ≤1 s; save/load mid-scene → no residue, director resumes; fast-travel away → cleanup via detach/stall path.
- Immersion checklist: hotkey sit → Space stands up → ledger restores; wheel opens/picks/closes with and without a target; hostile target → paired emote refused; eat/drink/med each animate once with cooldown; browser hotkey works; `cgf "OSF.Health"` passes throughout; `logLevel: trace` shows no per-frame director cost when idle/disabled.

## Suggested order
Phase 0 spikes → Phase 1 foundations → Phase 2 director core (testable via `AmbientScanNow` pre-sinks) → director sinks + content (iterable via `ReloadPacks`, no recompile) → Phase 3 sit/lean → wheel → item-use → Phase 4 packaging/docs. Ambient Director ships first if splitting releases.

## API gaps recorded (follow-up, not blockers)
- `IOSFSceneAPI` lacks tag matchmaking / anchor-first start / native scene-end callback — MINOR-bump vmethod appends later so third-party native plugins can do what the director does.
