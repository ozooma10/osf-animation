# Overlay Runtime — RFC Phase 1 (Persistent Actor Overlay Controller)

> **Revised 2026-08-04.** This supersedes the first draft of this plan. The core direction is unchanged
> (overlays leave SceneRuntime and reuse the existing `Graph`/unanchored staged-playback path), but four
> areas were reworked after review: playback ownership, transition execution, handoff callbacks, and
> duration validation. See **Review notes** at the end for what changed and why.
>
> **Implementation amendment.** Before ABI freeze, route start was made structured, external prop
> callbacks were split into typed attach/destroy events, and unsupported marker/sound lifetimes were
> removed. Phase-1 scene suspension is explicitly immediate because the one-stamper path cannot
> crossfade an overlay with an incoming scene. The shipped contract is authoritative in
> [`ROUTE_SCHEMA.md`](ROUTE_SCHEMA.md) and [`API.md`](API.md).

## Context

OAR/DAR-style overlay animations (Suit Protocol helmet routes, emote idles that play over the player's live state) are currently authored as full scenes with every policy flag manually disabled (`stripActors:false, lockPlayer:false, camera:"none", inPlace:true, playerControl:false, clearHeldItems:false` + role `mask`). This is dangerous (forget one flag → the player gets stripped or teleported) and structurally limited: the overlay consumes the actor's single scene slot (`SceneRuntime::MintSlot`), every node needs a playable (no zero-animation "stowed" station), and there is no owner-scoped suspend/resume around real scenes.

Decision (user-confirmed): implement **Phase 1 of `docs/RFC-actor-animation-runtime.md`** — overlays stop being scenes and get their own controller. Primary consumers are external plugins (Suit Protocol) via the native C ABI. Condition-driven (declarative OAR/DAR-style) playback is a later layer that will drive this same controller; Phase 1 keeps that door open but ships API-first.

**Key deviation from the RFC's literal text (justified):** overlay transitions play as one-actor, `anchored=false` staged plans via the existing `GraphManager::PlaySceneStaged` — not a new sampler and not the upgraded solo path. The RFC's "current standalone playback is not sufficient" bullet predates commits `4975148`/`7327b42`/`c099f8a`; per-stage masks, feathered live-base capture, timed marks, hold stages, and auto-end all already live on the scene path. The one-stamper rule is untouched (same per-actor `Graph`). `ClipInstance`/`PoseCompositor` stay Phase 2.

Phase 1 supports **one live route instance per actor** — including zero-animation stations. A second route returns `busy`; simultaneous layers remain Phase 2.

## Runtime and playback

New Layer B service `OverlayService` (peer of SceneRuntime, `src/Overlay/`) with owner records, generational route handles, and one `OverlayController` per live route. Controller state is split into two orthogonal axes:

- **Phase:** at-station, transitioning, failed.
- **Blocker:** none, scene, actor-3D unavailable.

**One transition edge at a time.** Each playback contains the edge plus an optional destination hold stage, with its clip and destination pose loaded before the edge starts. At the internal edge-end mark, commit `reached`, clean transition props, then either remain at the station or start the next BFS edge. BFS uses shortest edge count with authored-order tie-breaking. (Trade-off vs. compiling the whole path into one multi-stage plan: simpler countermand/reroute semantics and per-edge preload, at the cost of a playback boundary — fresh live-base capture — per edge. Seam quality across multi-edge chains is an explicit in-game acceptance criterion below.)

**Interruption is authored:**

- `finish` is the default: a countermand records the new destination, finishes the current edge, then reroutes.
- `crossfade-before-commit` may replace playback only before the commit checkpoint.
- Reverse movement uses ordinary authored reverse edges; Phase 1 does not scrub clips backward.
- A countermand after commit always finishes the edge first (the handoff already happened).

**Checkpoint vs. reached.** The controller stores both `reachedStation` and `checkpointStation`. A successful external handoff advances the checkpoint to the transition destination even if the pose has not finished; suspension resumes from that ownership-safe checkpoint. (This prevents replaying an already-acknowledged handoff — e.g. helmet visible, head hidden — after a mid-edge suspension.)

**Zero-animation stations** (Head, Stowed): no plan at all — stop/fade the graph, keep only controller state + external-lifetime prop refs. A stowed helmet costs zero graph and zero scene slot — but it still occupies the actor's single overlay slot (see Assumptions).

**Playback sinks replace the single-handler setters.** `GraphManager`'s `SetSceneAutoEndHandler`/`SetSceneClearHandler`/`SetSceneTimedMarkHandler` (`src/Animation/GraphManager.h:28/33/38`) become registered playback sinks: each staged playback stores its sink ID, and timed marks/auto-end route directly to their owner — no registrant ordering, no guessing by `PlaybackId`.

**Cross-owner admission in `PlaySceneStaged`.** Today `PlaySceneStaged` silently tears down any existing scene on an actor (`GraphManager.cpp:479` "already in a scene — stopping it first"). With two services sharing the entry point that's a desync hazard: SceneRuntime, OverlayService, and standalone playback must not silently clobber one another. Stops and replacements require the expected `PlaybackId`; the solo paths (`PlayDecodedAnimation`/`PlayAnimationBytes`) get the same overlay guard.

**Strict timed-mark validation (route-only), post-decode.** Registry load frequently doesn't know clip durations (`SceneRegistry.h:216` — `0 = unknown, probe at runtime`), and current scene behavior lets a frame past the clip end silently never fire (`SceneRegistry.cpp:439`). Silent skip is tolerable for scene cues but not for a route's commit marker — a skipped mark means an ownership handoff never happens. Routes therefore validate numeric marks after clips decode but **before** graph mutation; a mark at or past the actual duration rejects the request without disturbing existing playback.

**Actor 3D loss.** On `TESObjectLoadedEvent`, suspend unavailable actors and reconcile pending routes when their 3D returns. Reasserting a request remains an idempotent fallback (reassert-to-converge).

## Route schema

New top-level `routes` section in the unified `*.osf.json` files, parsed by `SceneRegistry` into `RouteDef`/`RouteRef` with independent `RouteRef` lifetime pinning and per-file accepted/declared/rejected route counts in `SceneFileStats`.

```jsonc
{ "routes": [{
    "id": "suitprotocol.helmet",
    "stations": [
      { "id": "head" },
      { "id": "held", "layer": { "clip": ".../head_to_held.af", "holdAt": 1.0, "mask": "upperBody", "mode": "override" } },
      { "id": "stowed" } ],
    "transitions": [
      { "id": "head-to-held", "from": "head", "to": "held",
        "layer": { "clip": ".../head_to_held.af", "mask": "upperBody" },
        "commit":  { "atFrame": 24, "marker": "suitprotocol.helmet.hide_head" },
        "markers": [ { "atFrame": 24, "id": "suitprotocol.helmet.hide_head" } ],
        "props":   [ { "attachAtFrame": 24, "prop": "helmet", "lifetime": "transition" } ],
        "sound":   [ { "atFrame": 23, "spec": "..." } ] } ] }] }
```

Rules:

- **Masks are mandatory.** Every animated station or transition requires a named `BoneMask`; unmasked actor-wide route playback is a validation error. Root/world motion is always engine-owned and `anchored=false` is forced internally. (A near-full-body overlay — e.g. an emote idle — authors a full-body-minus-root mask explicitly.)
- **Frame timing is fixed at 30 fps** (the file-level `atFrame` convention); there is no route-level `fps`.
- **`commit` replaces `eventFrame`:** one optional object holding the commit position and its external marker ID, so the ownership handoff is tied to an explicit marker rather than a bare frame number.
- **Prop lifetimes are explicit:** `transition`, `station`, `controller`, or `external`. Markers are instantaneous and sounds are one-shot, so neither accepts an inert lifetime promise. Scene suspension destroys every OSF-owned visual prop; external objects are never mutated, and typed attach/destroy callbacks tell the owner what to do.
- **Policy keys are rejected** (`stripActors`, `lockPlayer`, `camera`, `inPlace`, `fade`, `playerControl`, `clearHeldItems`, `roles`) — the overlay runtime has no policy mechanisms, so the flag-soup bug class disappears. **`claims`, `conditions`, and `when` are also rejected in schema v1** — no inert fields published before Phase 2 arbitration exists; reject-now-accept-later is the safe compatibility direction.
- Registry load validates duplicate IDs, endpoints, masks, prop definitions, timing syntax, and layer shape. A directed route **may** contain unreachable pairs (one-way transitions are legitimate); an actual request with no path returns structured `noPath` instead.

Migration: existing compiled-route scenes stay valid; Studio's route compiler gains a `routes` output target emitting the same marker IDs the cue lanes carry today, so packs can dual-ship. `examplescenes.json` gets a sibling route-form example.

## Native C ABI (`src/API/OSFOverlayAPI.h` + `.cpp`, export `OSF_RequestOverlayAPI`, v1.0)

Follows `OSFSceneAPI.h` conventions (size-prefixed structs, generational int32 handles, game-thread mutators):

- `AcquireOwner(pluginId, callback, ctx) → uint64` / `ReleaseOwner` (callback-barrier semantics)
- `BeginRoute(owner, actor, routeId, initialStation) → { routeHandle, disposition, reason }`
- `RequestStation(handle, station, token) → {accepted|pending|rejected}` with stable reason enums
- `EndRoute(handle, fade)` / `GetRouteForActor` / `QueryRoute`

One active owner record per plugin ID. Owner registrations survive world replacement; route handles and actor references do not.

**Owner-scoped callback, not a broadcast registry.** Instead of cloning `NativeSceneEventRegistry`, each owner registers one callback. The callback returns acknowledge/reject for **commit** events (an unacknowledged handoff must not be assumed applied); informational event results are ignored; an exception rejects the handoff. Events carry route handle, actor, route/stations/transition, typed marker or prop payload, request token, transition generation, outcome, and reason. Reentrant mutators are queued until dispatch completes; a reentrant begin returns `pending/dispatchDeferred`, and `ReleaseOwner` remains a callback barrier.

Sound-pool resolution, gender substitution, emitter selection, and subtitles are factored out of SceneRuntime so scenes and overlays share identical sound behavior. *(Deferral candidate — see Review notes.)*

Suit Protocol migrates from `StartSceneRoles("suitprotocol.route.helmet.*")` + cue matching to `BeginRoute` + `RequestStation` + marker events with the same IDs; its state machine (atmosphere, prefs, transactions, persistent clone) stays in the consumer. Studio preview lease (`SuitProtocolPreviewAPI.h`) untouched. No Papyrus surface in Phase 1.

## Scene integration

- **Scene start** on an overlaid actor: after `MintSlot` succeeds and before `PlayNodeAnim` in the start funnels (`src/Scene/SceneRuntime_Graph.cpp:736`, `:918`, and the other `StartFrom*` paths), batch-suspend participant overlays using the scene handle. Scene always wins (Phase 1 overlays are cosmetic), and suspension stops the overlay immediately; the one-stamper path cannot blend it against the incoming scene.
- **Start failure:** release the scene slot, then reconcile the suspended overlays (covers the failure-rollback `ReleaseSlot` paths at `:763`, `:924`).
- **Normal end:** stop the expected scene playback, replay the ledger and dispatch scene end, release the slot (`SceneRuntime_Graph.cpp:686`), then reconcile overlays — matching the existing end-path ordering exactly.
- Requests received during a scene update `desiredStation` and return `pending` without touching unavailable props or playback; reconcile on resume from the checkpoint.
- **World-replacing load:** clear controllers and OSF-owned props with **no overlay callbacks** (matches `StopAll`'s no-callback teardown policy); consumers rebuild from semantic state using their surviving owner handle. **Save:** nothing — overlays never set `kAnimationDriven`.

## Delivery gates (ordered)

1. Registry contract and fixtures (`RouteDef`/`RouteRef`, parse/validate `routes`, snapshot map, route counts; `test/fixtures/Data/OSF/fixture_route.osf.json` + `fixture_route_errors.osf.json`).
2. GraphManager sink/admission changes (playback sinks, `PlaySceneStaged` cross-owner admission, solo-path overlay guard, confirm `anchored=false` plans skip placement/`kAnimationDriven`).
3. Engine-free route planner and controller state machine (`src/Overlay/RoutePlan.{h,cpp}`, controller behind a tiny playback interface; `test/unit/test_route_plan.cpp`).
4. `OverlayService`, props/sound, scene suspension hooks, and the C ABI; wire in `src/main.cpp` after `SceneRuntime::RegisterWithGraphManager()`.
5. Browser import-count updates and documentation (route schema doc, RFC Phase 1 status flip, `AGENTS.md` Layer B entry, CHANGELOG).
6. External: Studio compiler `routes` output and Suit Protocol dual-shipping — feature-detect the new API, retain the scene fallback during validation.

Deferred (not Phase 1): multi-layer compositor / overlays *during* scenes, condition evaluator, claims arbitration, `required` enforcement, restraint phase, engine clip replacement, Papyrus, browser UI beyond import counts.

## Tests and acceptance

Engine-free (authored on the Mac dev box, run via `xmake test` on the Windows box/CI):

- Registry: forbidden policy keys, mandatory masks, duplicate IDs, endpoints, commit syntax, prop lifetimes, route-local rejection.
- Fake-playback controller: deterministic routing, one-edge execution, pre/post-commit countermands, stale generations, acknowledgement failure, idempotent reassertion, `noPath`/`busy` outcomes, scene suspension, 3D loss, owner release, world clear.
- Playback-sink/admission: standalone, scene, and overlay owners cannot replace one another; strict marks reject before graph mutation.
- Callback machinery: self-release, cross-thread barriers, reentrant queuing, exception isolation, acknowledgement propagation.
- After import-report UI changes: `npm --prefix ui/animation-browser run verify`.

In-game acceptance (Windows session):

- Held pose follows live locomotion indefinitely (feathered seam).
- Head/Stowed stations use no graph and no SceneRuntime slot; actor is fully vanilla.
- **Multi-edge chains look continuous** — no visible seam or pop at edge boundaries (each edge is its own playback with fresh live-base capture; this is the cost of one-edge-at-a-time and must be verified, not assumed).
- Each ownership commit occurs exactly once or fails with cleanup.
- Scene start/end suspension ordering is deterministic on the player.
- Missing 3D reconciles without replaying stale markers.
- Studio/standalone playback cannot silently replace an overlay.
- Prop attach/destroy at markers; save mid-Held → load → consumer reassert; Suit Protocol end-to-end; existing scenes, timed lanes, and save/load teardown unchanged (run emote/immersion packs).

## Assumptions

- One route instance occupies an actor even at a zero-animation station; consumers must end the route before another owner can begin (a second route returns `busy`). **Consumer-visible caveat:** Suit Protocol parked at `stowed` blocks other owners' overlays; ending the route to free the slot discards controller state. Accepted as the Phase 1 line (simultaneous layers are Phase 2) — document loudly for consumers.
- Scenes always win over cosmetic overlays.
- Phase 1 targets the human third-person/full-body skeleton only.
- Overlays persist in memory during a loaded world but are never serialized.
- Simultaneous overlays, claims arbitration, declarative conditions, Papyrus, and scene-time composition remain Phase 2 or later.

## Condition-driven future

Everything funnels through one internal `OverlayService::RequestStationInternal(owner, actor, route, station, token)`. A later `ConditionEvaluator` (declarative `when:` clauses over equip events / asserted conditions) becomes just another owner calling the same funnel — no controller changes. Suit Protocol can become *partially* declarative: presentation (which station to show for a given fact) can be JSON, but sealed/unsealed decisions depend on atmosphere queries, player prefs, and save-persistent gameplay truth that OSF's content-neutrality deliberately excludes. Realistic end state: Suit Protocol shrinks to a thin semantic driver asserting facts; a zero-plugin version is a non-goal.

## Review notes (2026-08-04 revision vs. first draft)

Evaluation of the revision against the codebase. Verdict: the revision is an improvement, not churn — most deltas fix things the first draft got wrong against the actual code.

**Verified fixes (grounded in code):**

- *`PlaySceneStaged` admission* — the strongest addition. The current code silently tears down an existing scene on the actor (`GraphManager.cpp:479`); the first draft only guarded the solo paths and relied on hook ordering. With two services sharing the entry point, expected-`PlaybackId` admission is structural protection the draft lacked.
- *Timed-mark validation moved to post-decode* — the draft's "markers past clip duration are load errors" was infeasible: durations are often unknown at registry load (`SceneRegistry.h:216`), and current behavior silently skips late marks (`SceneRegistry.cpp:439`). Silent skip is fatal for a commit marker; post-decode/pre-mutation rejection is the correct pipeline point.
- *Unreachable stations → per-request `noPath`* — the draft's load-time reachability requirement would forbid legitimate one-way routes.
- *Rejecting `claims` instead of parsing inert* — avoids freezing semantics before Phase 2 arbitration exists.
- *`checkpointStation`/`reachedStation` split* — closes a real hazard: resuming from a pre-commit station after an acknowledged handoff would replay the handoff or desync the consumer.
- *3D-loss reconciliation* — the draft didn't address actor 3D unload at all; it matters for NPC overlays across cell transitions.

**Behavior changes consciously accepted:**

- *Default interruption is `finish`, not immediate crossfade.* More predictable and safer around handoffs, but a countermand early in a long edge now waits the edge out unless the author opts into `crossfade-before-commit`. This is a felt responsiveness change, not a neutral restructuring.
- *Zero-animation stations occupy the overlay slot* — qualifies the "stowed costs nothing" headline win; see Assumptions.

**Scope-risk watchlist:**

- *One-edge-at-a-time* is the biggest architectural delta: it trades the staged path's pointer-swap stage switches (no re-capture, no seam) for simpler countermand semantics. The seam risk is real and is now an explicit in-game acceptance criterion.
- *Playback sinks* are the cleaner architecture but touch every dispatch site — gate 2 is larger than the draft's "(S)" estimate implied.
- *Ack/reject callback machinery* (reentrancy queue, exception-rejects-handoff, `ReleaseOwner` barrier) is the most complex new subsystem and where the schedule risk lives; the test list covers it deliberately.
- *Sound factoring out of SceneRuntime* is the one deferral candidate: right long-term, but it refactors working scene code inside an already-large gate 4. Overlays could ship v1 with a narrower sound path and unify later.
