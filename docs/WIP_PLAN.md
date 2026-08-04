# Overlay Runtime — RFC Phase 1 (Persistent Actor Overlay Controller)

## Context

OAR/DAR-style overlay animations (Suit Protocol helmet routes, emote idles that play over the player's live state) are currently authored as full scenes with every policy flag manually disabled (`stripActors:false, lockPlayer:false, camera:"none", inPlace:true, playerControl:false, clearHeldItems:false` + role `mask`). This is dangerous (forget one flag → the player gets stripped or teleported) and structurally limited: the overlay consumes the actor's single scene slot (`SceneRuntime::MintSlot`), every node needs a playable (no zero-animation "stowed" station), and there is no owner-scoped suspend/resume around real scenes.

Decision (user-confirmed): implement **Phase 1 of `docs/RFC-actor-animation-runtime.md`** — overlays stop being scenes and get their own controller. Primary consumers are external plugins (Suit Protocol) via the native C ABI. Condition-driven (declarative OAR/DAR-style) playback is a later layer that will drive this same controller; Phase 1 keeps that door open but ships API-first.

## Design summary

New Layer B service `OverlayService` (peer of SceneRuntime, `src/Overlay/`), owning at most **one `OverlayController` per actor**. Overlays are authored as **routes** (stations + transitions) — a new `"routes": [...]` top-level section in the unified `*.osf.json` files, parsed by `SceneRegistry` into `RouteDef`/`RouteRef`. Policy flags (`stripActors`, `lockPlayer`, `camera`, `inPlace`, `fade`, `playerControl`, `clearHeldItems`, `roles`) are **validation errors** on routes — the overlay runtime has no policy mechanisms to engage, so the whole flag-soup class of bugs disappears.

**Key deviation from the RFC's literal text (justified):** overlay transitions play as one-actor, `anchored=false` staged plans via the existing `GraphManager::PlaySceneStaged` — not a new sampler and not the upgraded solo path. The RFC's "current standalone playback is not sufficient" bullet predates commits `4975148`/`7327b42`/`c099f8a`; per-stage masks, feathered live-base capture, timed marks, hold stages, and auto-end all already live on the scene path. The one-stamper rule is untouched (same per-actor `Graph`). `ClipInstance`/`PoseCompositor` stay Phase 2.

### Runtime
- `OverlayController` state: owner handle, generational overlay handle, `RouteRef`, desired/reached station, last checkpoint, state (`kAtStation | kTransitioning | kSuspendedByScene | kFailed`), active `PlaybackId` + transition generation, prop instances with lifetimes (`transition|station|controller|external`).
- Engine-free `src/Overlay/RoutePlan.{h,cpp}`: BFS over transitions from reached → desired station, compiling one `Animation::ScenePlan` (one stage per edge, final `hold:true` stage for station poses; markers/prop actions/sound compiled into stage lanes; masks/mode/weight/preserveBones from the route's layer contract; root always engine-owned).
- **Zero-animation stations** (Head, Stowed): no plan at all — stop/fade the graph, keep only controller state + external-lifetime prop refs. This is the headline win: a stowed helmet costs zero graph and zero scene slot.
- Request lifecycle: `RequestStation` → compile + play + `kTransitionStarted`; countermand before the edge's ownership marker supersedes immediately (crossfade), after it finishes the edge then reroutes; while suspended → record desired, return `pending`, reconcile on resume; failure → structured `kFailed(reason)`, stay at last reached station. Reasserting the current desired station is always safe (reassert-to-converge).
- Events (synchronous, game thread, mirroring `SceneEventRelay`): `kTransitionStarted, kMarker(id), kStationReached, kSuspended, kResumed, kInterrupted, kFailed` carrying owner/actor/route/station/transition/requestToken.

### Coexistence with SceneRuntime
- **Scene start** on an overlaid actor: hook after `MintSlot` succeeds and before `PlayNodeAnim` in the start funnels (`src/Scene/SceneRuntime_Graph.cpp:736`, `:918`, and the other `StartFrom*` paths) → `OverlayService::SuspendForScene(participants)`: stop overlay playback, destroy transition/station-lifetime props, retain desired/reached/checkpoint, fire `kSuspended`. Scene always wins (Phase 1 overlays are cosmetic).
- **Scene end**: after `ReleaseSlot` in `ApplyTransition`'s end path (`SceneRuntime_Graph.cpp:686`) → `OverlayService::ResumeAfterScene(participants)`: re-enter desired station from last checkpoint, recreate station props, fire `kResumed`. Also cover the `StartFrom*` failure-rollback `ReleaseSlot` paths (`:763`, `:924`).
- **World-replacing load**: register with the scene-clear handler — drop all controllers/props, no callbacks (matches scene teardown policy; consumers reassert, already Suit Protocol's model). **Save**: nothing — overlays never set `kAnimationDriven`.
- **Solo playback guard**: `GraphManager::PlayDecodedAnimation`/`PlayAnimationBytes` refuse an actor with an active overlay (mirrors the existing scene-participant refusal) unless the caller is OverlayService. Studio preview and an overlay can't share the player — acceptable for Phase 1.

### GraphManager changes (small)
`src/Animation/GraphManager.h:28/33/38` — `SetSceneAutoEndHandler`/`SetSceneClearHandler`/`SetSceneTimedMarkHandler` each hold a single `std::function`; multiplex to two registrants (auto-end handlers tried in order until one claims the `PlaybackId`; timed-mark and clear notify all). Ownership already resolves by `PlaybackId`, so SceneRuntime naturally ignores overlay playbacks and vice versa.

### Schema (routes)
```jsonc
{ "routes": [{
    "id": "suitprotocol.helmet", "fps": 30,
    "stations": [
      { "id": "head" },
      { "id": "held", "layer": { "clip": ".../head_to_held.af", "holdAt": 1.0, "mask": "upperBody", "mode": "override" } },
      { "id": "stowed" } ],
    "transitions": [
      { "id": "head-to-held", "from": "head", "to": "held",
        "layer": { "clip": ".../head_to_held.af", "mask": "upperBody" },
        "eventFrame": 24,
        "markers": [ { "atFrame": 24, "id": "suitprotocol.helmet.hide_head" } ],
        "props":   [ { "attachAtFrame": 24, "prop": "helmet" } ],
        "sound":   [ { "atFrame": 23, "spec": "..." } ] } ] }] }
```
Reuses the file-level `props` registry, `atFrame`@30fps convention, named `BoneMask` ids, `poseMode`/`poseWeight`/`preserveBones`. Validation: policy flags rejected; unknown endpoints/unreachable stations/unknown masks/markers past clip duration are load errors. `claims` parsed + validated against the RFC claim table but not arbitrated (forward-compat with Phase 2). `conditions`/`when` reserved (rejected as unknown today).

Migration: existing compiled-route scenes stay valid; Studio's route compiler gains a `routes` output target emitting the same marker ids the cue lanes carry today, so packs can dual-ship. `examplescenes.json` gets a sibling route-form example.

### C ABI (`src/API/OSFOverlayAPI.h` + `.cpp`, export `OSF_RequestOverlayAPI`, v1.0)
Follows `OSFSceneAPI.h` conventions (size-prefixed structs, generational int32 handles, game-thread mutators):
`AcquireOwner(pluginId, callback, ctx) → uint64` / `ReleaseOwner` (callback-barrier semantics) / `BeginRoute(owner, actor, routeId, initialStation) → int32` / `RequestStation(handle, station, token) → {accepted|pending|rejected}` / `EndOverlay(handle, fade)` / `GetOverlayForActor` / `QueryOverlay`. Event registry: new `src/API/NativeOverlayEventRegistry.{h,cpp}` cloned from `NativeSceneEventRegistry` (engine-free, lease-based unregister).

Suit Protocol migrates from `StartSceneRoles("suitprotocol.route.helmet.*")` + cue matching to `BeginRoute` + `RequestStation` + marker events with the same ids; its state machine (atmosphere, prefs, transactions, persistent clone) stays in the consumer. Studio preview lease (`SuitProtocolPreviewAPI.h`) untouched in Phase 1. No Papyrus surface in Phase 1 (deferred).

### Condition-driven future (user's question answered)
Everything funnels through one internal `OverlayService::RequestStationInternal(owner, actor, route, station, token)`. A later `ConditionEvaluator` (declarative `when:` clauses over equip events / asserted conditions) becomes just another owner calling the same funnel — no controller changes. Suit Protocol can become *partially* declarative: presentation (which station to show for a given fact) can be JSON, but sealed/unsealed decisions depend on atmosphere queries, player prefs, and save-persistent gameplay truth that OSF's content-neutrality deliberately excludes. Realistic end state: Suit Protocol shrinks to a thin semantic driver asserting facts; a zero-plugin version is a non-goal.

## Implementation checklist (ordered)

1. **(M)** Registry: `RouteDef`/`RouteRef`, parse/validate `routes` section, snapshot map, `SceneFileStats` route counts — `src/Registry/SceneRegistry.{h,cpp}`; fixtures `test/fixtures/Data/OSF/fixture_route.osf.json` + `fixture_route_errors.osf.json`; tests per existing scene-registry suite pattern.
2. **(M)** Engine-free `src/Overlay/RoutePlan.{h,cpp}`: BFS + `ScenePlan` compilation + lane layout + countermand rules; `test/unit/test_route_plan.cpp`.
3. **(S)** GraphManager: multiplex the three handlers; overlay guard in `PlayDecodedAnimation`; confirm `anchored=false` plans skip placement/`kAnimationDriven` — `src/Animation/GraphManager.{h,cpp}`.
4. **(L)** `src/Overlay/OverlayService.{h,cpp}`: owner table, controllers, Begin/Request/End, mark decode (props via `Props::PropService`, sound via SoundService), zero-anim stations, suspend/resume/reconcile, world-load clear; wire in `src/main.cpp` after `SceneRuntime::RegisterWithGraphManager()`.
5. **(M)** SceneRuntime hooks: suspend after `MintSlot` in start funnels; resume after `ReleaseSlot` in `ApplyTransition` end path + failure-rollback paths — `src/Scene/SceneRuntime_Graph.cpp`.
6. **(M)** C ABI: `OSFOverlayAPI.{h,cpp}`, `NativeOverlayEventRegistry.{h,cpp}`, export, `docs/API.md`.
7. **(M)** Studio compiler `routes` target + dual-ship the Suit Protocol pack; route-form example beside `examplescenes.json`.
8. **(S)** Docs: route schema section (`docs/SCENE_SCHEMA.md` or new `docs/ROUTE_SCHEMA.md`), RFC Phase 1 status flip, `AGENTS.md` Layer B entry, CHANGELOG.

Deferred (not Phase 1): multi-layer compositor / overlays *during* scenes, condition evaluator, claims arbitration, `required` enforcement, restraint phase, engine clip replacement, Papyrus, browser UI beyond import counts.

## Verification

Dev box is a Mac — no compile, no game. Engine-free (authored here, run via `xmake test` on the Windows box/CI):
- Route registry fixtures: policy-flag rejection, bad endpoints, unreachable stations, unknown masks, markers past clip end.
- `RoutePlan` compiler: path BFS, stage/mark layout, countermand generation (pure functions).
- `NativeOverlayEventRegistry`: clone of `test/NativeSceneEventTest.cpp`.
- Controller state machine: factor transitions behind a tiny playback interface so request/suspend/resume/fail/checkpoint logic tests engine-free.

In-game Windows checklist (for the eventual session): scene start/end suspension ordering on the player; zero-animation station leaves the actor fully vanilla; prop attach/destroy at markers; hold-at-end station pose over live locomotion (feathered seam); save mid-Held → load → consumer reassert; Suit Protocol end-to-end; Studio-preview/overlay mutual-exclusion guard; multiplexed handlers don't regress normal scenes (run emote/immersion packs).
