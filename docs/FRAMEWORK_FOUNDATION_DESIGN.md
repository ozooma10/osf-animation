# Framework foundation: core runtime changes

Status: design, not started. Branch `dev`. Written 2026-10-15; source-level review applied the
same day (anchor flag split, actor-keyed baseline, API handshake, speed routing, single alignment
layer, handoff lane/ledger/input/anchor contracts, positional snap accepted).

Higher-level frameworks (encounter directors, gameplay mods that chain scenes, player
alignment tools) need a handful of things the scene runtime does not provide today. All of it is
content-neutral engine work and ships in OSF Animation itself. Nothing here adds adult
vocabulary, persistent gameplay state, or a second scene model.

| # | Change | Size | Why a framework needs it |
|---|---|---|---|
| 0 | Freeze the resolved world anchor per slot; actor-keyed baseline | S | Prerequisite for 3 and 4; fixes a latent drift and a wrong restore |
| 1 | Persistent scene speed | S | Speed silently resets on every node entry |
| 2 | Termination cause + scene id on `SCENE_END` | S | Callers cannot tell complete from cancel from interrupt |
| 2b | Engine API minor-version negotiation | S | Every later item appends vmethods; today's handshake rejects any bump |
| 3 | Alignment overrides (live nudge + persisted deltas) | M | Misaligned casts are the first thing players see |
| 4 | Scene handoff (same cast, new scene, same handle) | L | "Change animation", "swap roles", chained encounters |

Everything is additive under the API stability policy (`docs/API.md`, "API stability policy"):
new natives, appended `SceneEvent` fields, appended engine-API vmethods. One behavior change is
called out in §1. §2b makes the "appended vmethods" claim actually true at runtime.

Vocabulary follows `docs/VOCABULARY.md`: scene, flow node, linear stage, playback segment,
participant, role, world anchor, world placement.

---

## 0. Freeze the resolved world anchor per slot

### Problem

When a start has no explicit anchor, every node entry re-samples participant 0's live
`data.location` / `angle.z` as the anchor (`GraphManager.cpp` `BuildPlaybackFromPlan`,
around lines 253-259). The per-frame pin has already written participant 0's **offset-applied**
position into `data.location`. So a role 0 with a non-zero `offset` shifts the whole scene by that
offset on every node transition. Any live nudge of participant 0 (§3) would do the same.

`PlayNodeAnim` (`src/Scene/SceneRuntime_Graph.cpp:221-277`) only sets `plan->anchorExplicit`
when `Slot::anchor.set`, and `anchorExplicit` also enables transform restore at stop
(`GraphManager.cpp` around line 236), so it cannot double as "anchor is frozen".

### Design

**Split the plan flag.** `BuildPlaybackFromPlan` uses `plan.anchorPos/Heading` only when
`anchorExplicit` is set (`GraphManager.cpp` around line 253), and the same flag also arms
`restoreParticipantTransforms`. Supplying frozen coordinates without the flag would still sample
participant 0. Replace the one flag with two on `PlaybackPlan`:

- `anchorResolved`: use `anchorPos/Heading` instead of sampling participant 0.
- `restoreTransformsOnEnd`: arm the end-of-playback restore.

Today's `anchorExplicit` callers set both. The frozen-anchor path sets only `anchorResolved`.

**Freeze.** After the first successful `PlaySynchronized` in `SceneRuntime::Start`, copy the
session's resolved `anchorPos/Heading` into `Slot::frozenAnchor`. `PlayNodeAnim` applies it with
`anchorResolved` whenever `Slot::anchor` is unset. Explicit anchors keep today's path.

**Baseline keyed by actor, captured for every start.** `originalTransforms` is captured only
when `anchored && anchorExplicit` and is indexed by participant position
(`GraphManager.cpp` around lines 238 and 1173). That breaks in two ways a framework will hit:
a start without an explicit anchor has no baseline for a later anchored handoff, and a role swap
(§4) would restore each actor to the other's spot. So:

- `SceneRuntime::Start` captures `Slot::baseline` as `vector<pair<Actor*, transform>>` for all
  participants **before** the first play, regardless of anchoring.
- `PlayNodeAnim` stamps `plan->baselineTransforms` on every node entry, reordered to the
  **current** participant order by actor identity. The field exists
  (`src/Animation/Scene.h:155-157`) and has had no writer since the preview service was removed,
  so explicit-anchor graph scenes currently restore to the previous node's placement.
- Restore semantics are unchanged: it still fires only for anchored, explicit-anchor playbacks.
  The baseline is simply correct and available when it does.

### Acceptance

- A two-role linear scene with `roles[0].offset.x = 50` plays five stages without the cast
  creeping. Verify via participant positions at each `NODE_ENTER`.
- A furniture-anchored graph scene restores both actors to their pre-scene transforms on
  `StopScene` after three node transitions.
- After a `ReplaceSceneRoles` swap (§4) and a final stop, each actor returns to its **own**
  pre-scene transform.

---

## 1. Persistent scene speed

### Problem

- `PlaybackSession::speed` lives on the session. Each node entry builds a new session at
  `plan.speed = 1.0`, so Advance, `SetSceneStage`, timer edges and auto-advance all reset speed.
  Linear stages are synthetic nodes, so this hits every multi-stage scene.
- `kPause` keeps `s_prePauseSpeed` in one static (`src/Scene/SceneRuntime.cpp` around line 28),
  shared by every scene.
- Papyrus registered starts ignore `SceneOptions.Speed` (`src/Scene/SceneLauncher.cpp:108-116`);
  native `StartScene` applies it to the entry node only.
- There is no by-handle Papyrus speed native; `OSF.SetSpeed` is by actor.
- The browser works around pause only (`extensions/browser/src/API/UIBridge.cpp:737-740`).

### Design

- Add `Slot::speed = 1.0f` and `Slot::prePauseSpeed = 0.0f`.
- Seed `Slot::speed` from the launch speed in **every** start path, including Papyrus
  `SceneOptions.Speed` for registered scenes. **Behavior change:** registered Papyrus starts
  begin honoring `Speed`. Document in `UPGRADING.md`; a caller that set `Speed` on a registered
  start today gets a no-op, so breakage risk is low.
- `PlayNodeAnim` stamps `plan->speed = slot.speed` next to the existing `loopScale` /
  `worldPlacement` re-application.
- One authoritative setter: `SceneRuntime::SetSpeed(handle, v)` writes `Slot::speed` and the
  live session. `SetSceneSpeed` (engine services, already exists) calls it. `GetSceneSpeed` reads
  the slot. Hotkey verbs (`kSpeedUp/Down/Reset/Pause`) go through the same path keyed by
  `grant.handle`; delete the static.
- **By-actor setters route through the slot too.** `GraphManager::SetSpeed` writes only the
  session (`GraphManager.cpp` around line 870), so `OSF.SetSpeed(actor, 0.5)` would otherwise be
  lost at the next node and disagree with `GetSceneSpeed`. Keep the public signatures; when the
  actor is a participant, resolve the slot and call the authoritative setter. Solo graphs keep the
  sync-group path.
- New Papyrus natives: `OSF.SetSceneSpeed(int aiScene, float afSpeed)`,
  `float OSF.GetSceneSpeed(int aiScene)`.
- Clamps stay as they are (API 0..100, hotkeys 0.1..5, 0 = paused). Note the mismatch in API.md
  rather than changing it.
- Remove the browser pause workaround once this lands.

Node timers and loop counts already run on scaled time (`src/Animation/Scene.cpp:19`); speed
therefore also stretches authored timing. That is the desired behavior and should be stated in
API.md.

### Acceptance

- Set speed 0.5 on a five-stage linear scene, advance twice, `GetSceneSpeed` still 0.5 and the
  clip visibly plays at half rate.
- Pause scene A, start scene B on another cast, unpause A: A resumes at its own prior speed.
- `xmake test`: a plan-level test that plan building carries slot speed (factor the stamping into
  a pure helper so it is testable without the engine).

---

## 2. Termination cause and scene id on `SCENE_END`

### Problem

`Slot::terminationCause` (completed / cancelled / interrupted) is already tracked and logged in
`ApplyTransition` (`src/Scene/SceneRuntime_Graph.cpp:616-626`) but never emitted.
`VOCABULARY.md` documents the gap. `SceneEvent` also has no scene id, which §4 makes necessary
because a handle will be able to change scene mid-life.

### Design

- Append to `OSFSceneEvent` (size-prefixed, append-only) and to `OSFTypes:SceneEvent` (append at
  end):
  - `int cause` on `SCENE_END`: `OSF.CAUSE_COMPLETED()` = 0, `CAUSE_CANCELLED()` = 1,
    `CAUSE_INTERRUPTED()` = 2. `-1` on every other event type.
  - `string sceneId` on every event: the scene definition id the handle is running at dispatch.
- `Fire(kSceneEnd)` in `ApplyTransition` already has the cause in hand; pass it through.
- Unchanged: load teardown (`GraphManager::StopAll`) still emits nothing into the discarded world.
  Say so in API.md next to the new field.

### Acceptance

- Natural completion, `StopScene`, and a failed node play each deliver the matching cause to a
  registered Papyrus callback.
- Consumers compiled against the old `OSFTypes.psc` keep working (append-only).

---

## 2b. Engine API minor-version negotiation

### Problem

`docs/API.md` promises that minor versions only append, and §3 and §4 append vmethods to the
engine services table. But both ends of the handshake demand exact equality:
`OSF_RequestEngineAPI` returns null unless `version == kOSFEngineAPIVersion`
(`src/API/EngineServices.cpp:188`), and the browser's `Connect` requires
`api->version == kOSFEngineAPIVersion` (`extensions/browser/src/API/EngineClient.cpp:12`). The
browser ships as its own archive (`packaging/build-browser-addon.ps1`), so a user who updates
only the engine would lose the browser on the first minor bump.

### Design

- Engine: accept a request whose major matches and whose minor is `<=` the engine's. Return the
  full table; it is size-prefixed and append-only, so an older client reads a prefix.
- Client (browser, and any third-party consumer following the header's `RequestEngineAPI`
  helper): accept `major equal && api->size >= sizeof(table the client was compiled against)`.
  Update the inline helper in `OSFEngineAPI.h` so new consumers inherit the rule.
- Document the rule in API.md next to the stability policy and bump `kOSFEngineAPIVersion` to
  `0x00030001` with this change alone, so the negotiation itself is exercised before anything
  depends on it.
- The browser pins `kOSFUITested`-style expectations elsewhere; no change there.

### Acceptance

- The released 1.5.0 browser DLL connects to an engine built at minor `+1` and its existing
  calls work. A client compiled against minor `+1` refuses an engine at minor `0` with a logged
  reason rather than calling past the table end.
- `xmake test`: pure test of the accept/refuse predicate on both sides.

---

## 3. Alignment overrides

Players fix authored or converted offsets in-scene with hotkeys; the fix persists and applies the
next time that scene plays. Equivalent in scope to SexLab's alignment hotkeys, built on the
existing placement pipeline rather than a parallel one.

### What exists

- Authored placement: `roles[i].offset` and per-stage `clips[i].offset`, both
  `{x, y, z, heading}` (heading in degrees), resolved per stage per actor in
  `BuildPlanFromStages` (`src/Registry/ContentRegistry.cpp:2230`) into
  `PlaybackPlan::Segment::placements`.
- Runtime: the pin reads `Graph::scenePlacement` (`src/Animation/Graph.h:60-62`), written at
  playback start (`GraphManager.cpp` around line 550) and on every segment change
  (`src/Animation/Graph.cpp:394-405`). `PlacementToWorld` (`Scene.h:43-53`) is the only math.
- `PlaybackSession::stages` is immutable after publish; the session anchor is const after start.
- No nudge natives exist; `OSF.SetAnchor` is solo-graph only and refuses participants.

### Data model

Overrides are **deltas** on top of the authored placement, never absolutes, so a content update
that moves an authored offset keeps the player's correction meaningful.

Key: `(scene id, flow node id, role name)`. Linear stages key by their synthetic node id
(`#sN`), which is stable. A `use` node keys by the **host** scene and node, not the spliced
scene. A node with several inline segments gets one delta applied to all its segments.

Per-actor or per-race rows are a follow-up, not v1; the gear registry's `"player"` /
form-ref rows are the precedent if needed.

File: `Documents\My Games\Starfield\SFSE\OSF\alignment-overrides.json`, same `UserFilePath` lane
as `scene-gear.json`. Written by the engine with WheelPins-style tmp + rename. Loaded at
`kPostDataLoad` and inside `Content::ReloadAll` next to `Gear::LoadAll()`.

```json
{
  "formatVersion": 1,
  "scenes": {
    "author.scenes.demo": {
      "#s2": {
        "partner": { "x": 0, "y": -4.5, "z": 0, "heading": 3 }
      }
    }
  }
}
```

- Exactly one row per `(scene, node, role)`; no wildcard or fallback rows. A scene-wide default
  would make "zero for this one node" unrepresentable once zero rows are pruned. "Apply this
  delta to every node" is a save-time convenience (`kAlignSaveAll`, follow-up) that writes
  explicit rows.
- `heading` in degrees in the file (schema convention), radians at runtime.
- Missing components are 0. A row of all zeros is removed on save.
- Load failures follow the gear registry pattern: size cap, `ignore_comments`, per-document
  rollback, one health problem, scenes still play.

An optional pack-shipped lane (`Data/OSF/**/*.osfalign.json`, user file wins) can come later; it
is the mechanism by which a compatibility pack could ship community-fixed alignments.

### Runtime: one delta, one owner

There is exactly **one** correction value per `(node, role)` at runtime, owned by the slot. The
plan is never patched; the graph adds the delta on top of the authored placement. This rules out
accumulation (saved 5, nudged to 6, saved, revisited must give authored + 6, never + 11).

- `Slot::align`: `map<nodeId, array<ParticipantPlacement per role>>`, the working copy for this
  scene's lifetime.
- **Node entry** (`PlayNodeAnim`): if `Slot::align` has no entry for this node, seed it from the
  persisted table (or zeros). Then, after `PlaySynchronized`, push each role's delta to its graph
  via `GraphManager::SetAlignmentDelta(actor, delta)`. The plan carries authored placements only.
- **Graph side**: `Graph::alignDelta`, written under `g->stateLock`. `scenePlacement = authored +
  alignDelta` is recomputed in the setter and in `Graph::Sample`'s segment-change copy
  (`Graph.cpp` around line 403), so the delta survives segment switches inside the node. The pin
  is unchanged.
- **Nudge**: edits `Slot::align[node][role]`, then calls the setter. Pure addition of the step.
- **Save**: writes `Slot::align[node]` for the current node to the file (zero rows pruned).
  The working copy is unchanged.
- **Reset**: zeros `Slot::align[node][role]`, pushes it, and marks the row for removal on the
  next save. Nothing is written until save.
- **Node revisit** within the same scene: the working copy wins (unsaved nudges persist until
  scene end). This is the only state that outlives a node.
- **Scene end**: the working copy is discarded. Unsaved nudges are lost; the HUD line says
  "unsaved" while a node's working copy differs from the persisted row.
- **Reload** (`ReloadPacks`): replaces the persisted table. Live slots keep their working copy;
  the new table applies on the next node entry that has no working copy.
- **Handoff** (§4): the working copy is dropped with the old scene identity; the new scene's
  rows seed on its entry node.

The havok capsule is not moved during a nudge; it moved at the start teleport and the pin owns
the rendered root for the rest of the scene. If later testing shows capsule drift matters, re-post
the start teleport's `SetPosition` on save, not on every keypress.

Participant 0 nudges are safe only with §0 in place.

### Input

Scene controls gain an `align` capability on the reserved bit 3 (`src/Input/InputTypes.h:18`),
so `"sceneControls": { "disable": ["align"] }` works with no further plumbing. New verbs,
dispatched in `DispatchInputVerb` on the main thread, acting on the selected participant of
`grant.handle`:

| Verb | Default key | Effect |
|---|---|---|
| `kAlignSelect` | Numpad 5 | Cycle selected participant; HUD shows role name |
| `kAlignX` plus / minus | Numpad 6 / 4 | step along anchor-local X |
| `kAlignY` plus / minus | Numpad 8 / 2 | step along anchor-local Y |
| `kAlignZ` plus / minus | Numpad 3 / 1 | step on Z |
| `kAlignHeading` plus / minus | Numpad 9 / 7 | rotation step |
| `kAlignStep` | Numpad / | Toggle step 1.0 vs 0.1 units, 5 vs 1 degrees |
| `kAlignReset` | Numpad . | Clear this node's delta for the selected participant |
| `kAlignSave` | Numpad * | Write the file |

Numpad + and - stay speed. All keys become key-typed settings — playback in
`osf.animation.engine.json`, these twelve on their own `osf.animation.align.json` page — read in
`EngineSettings::RefreshSettings` via `GetKey` into atomics that `InputService::VerbForKeyboard`
consults. `kSceneKeys` names the owning schema per key, and a null one marks a fixed alias with no
schema row. That closes the hardcoded-keymap TODO at
`src/Input/InputService.cpp:116` for the existing verbs as well. Keep the engine's
`kPostDataLoad` settings init; do not switch to Settings `RegisterHotkey`, which fires outside the
scene grant.

Unsaved deltas are kept in the slot until save or scene end; scene end discards them. A HUD
debug line shows the current delta while aligning.

### Natives (additive)

```papyrus
bool    OSF.SetSceneAlignment(int aiScene, string asRole, float x, float y, float z, float headingDeg) ; live delta
float[] OSF.GetSceneAlignment(int aiScene, string asRole)                                            ; [x, y, z, headingDeg]
bool    OSF.SaveSceneAlignment(int aiScene)        ; persist current deltas for the current node
bool    OSF.ResetSceneAlignment(int aiScene, string asRole)
```

Equivalent vmethods appended to the engine services table (minor bump). The browser addon can
build a panel on these later; no browser work is part of this item.

### Acceptance

- Nudge the partner 5 units on Y in stage 2, save, stop, restart the same scene at stage 2:
  the partner stands at the nudged position. Stage 1 is unaffected.
- Edit `roles[].offset` in the content file, `ReloadPacks`, replay: the player delta still
  applies on top of the new authored offset.
- `"sceneControls": { "disable": ["align"] }` makes the numpad inert in that scene.
- Saved 5, nudge to 6, save, stop, replay the node: the partner sits at authored + 6. Nudge
  without saving, advance, go back to the node: the unsaved nudge is still applied.
- `xmake test`: delta parse/serialize round-trip with zero-row pruning; working-copy seeding and
  reset semantics as a pure state machine.

---

## 4. Scene handoff

Replace a live scene's definition with another one for the **same cast**, keeping the handle,
the ledger, the anchor, and the player's controls, with a pose blend instead of a stop/start.
This is what "change animation", "swap positions", and a director chaining authored scenes into
one encounter all reduce to.

### Why not stop then start

- After `StopScene`, the fading graphs stay mapped with no session; a sink-owned start on the same
  actors is admitted as `kStandaloneOccupied` until `RemoveFadedGraph` runs
  (`GraphManager.cpp:484-506`, `src/Animation/PlaybackAdmission.h:47-50`).
- `Fire(kSceneEnd)` always replays the ledger, so controls, camera, apparel and props flash back
  between scenes.
- Transform restore fires on the old scene's end.

### What exists

Every node transition is already a same-cast replacement: `ApplyTransition` calls
`PlayNodeAnim(..., expectedPlayback = old)`, which calls `PlaySynchronized` with
`replace = true`. `PrepareBlendSource` captures the on-screen pose, the old session's deferred
stop sees the new owner and skips AI-flag clear and restore, and the same `Graph` objects are
rebound. The ledger is untouched. `PlayNodeAnim` reads `def` from the slot. Node `use:` already
splices a foreign scene's stages inside the host slot.

### What a handoff promises, and what it does not

- **Pose**: one blend from the live pose into the new entry clip, via the existing replace
  admission.
- **Position**: a **snap**, not a glide. The replace path assigns the new placement and queues
  the start teleport (`GraphManager.cpp` around line 550), which is exactly what every node
  transition does today. Placement interpolation would help all transitions equally and is a
  separate follow-up; v1 accepts the snap. Authors who want a smooth position change hand off
  between scenes with matching entry placements.
- **Runtime policy**: the runtime itself releases nothing and adds no fade between the two
  scenes. Authored exit and enter lane actions still run and keep precedence (below).
- **Handle**: unchanged, one lifetime to consumers.

### Prerequisite: the slot must remember its anchor ref

`Slot::anchor` holds only a resolved position and heading (`OSFSceneAPI.cpp:48-61`,
`AnchorResolve.cpp:16-21`). A handoff into an anchor-bound scene needs the **ref** to run
`Matchmaking::AnchorAccepts(newDef, ref)` and to recompose the new definition's own
`anchorOffset` (`AnchorResolve.cpp:184-209`). Add `Slot::anchorRef` (`NiPointer<TESObjectREFR>`)
and `Slot::anchorBase` (the rendered base transform before any offset), filled by every start
path that has a ref. Starts that pass raw coordinates leave `anchorRef` null and can only hand off
to scenes without an anchor requirement.

### Design

`SceneRuntime::Replace(handle, newSceneId, roleNames | empty, ReplaceOptions)`:

1. **Validate under `_lock`.** Slot live and not `transitioning`; claim `transitioning`.
   New definition found. Participant set is identical (permutation allowed via `roleNames`).
   `RoleAccepts` passes for the new binding. Anchor compatibility:
   - new scene requires an anchor: `ReplaceOptions.anchor` or `Slot::anchorRef` must be set and
     `AnchorAccepts(newDef, ref)` must pass; the slot's anchor is recomposed from `anchorBase`
     with `newDef->anchorOffset`. Otherwise refuse with a logged reason.
   - new scene has no requirement: keep the slot's resolved or frozen anchor as is.
   - Follow-actor placement scenes hand off only to follow-actor scenes.
2. **Fire `NODE_EXIT`** on the old node. Exit lanes run as on any transition, and **authored
   actions keep precedence**: an authored `osf.control.release`, `osf.equipment.restore` or
   `osf.fade.out` on the exit lane does what it says (`SceneRuntime_Dispatch.cpp` `RunAction`).
   Step 5 then re-engages whatever the new scene's policy wants, in the same game-thread call,
   so a release followed by a re-lock is not observable. A fade is. Content that must never fade
   across a handoff should not author one on its exit lane; the runtime does not second-guess it.
3. **Swap identity.** Pin the new `SceneRef` snapshot into `s->definition`, set `s->id`,
   `s->node = entry` (honoring `ReplaceOptions.stage` like `SceneOptions.Stage`), reorder
   `s->participants` to the new role order, drop `Slot::align`, keep `frozenAnchor`,
   `anchorRef/anchorBase`, `baseline` (actor-keyed, so the reorder is safe), `speed`, `loopScale`,
   `worldPlacement`.
4. **Play.** `PlayNodeAnim(participants, newId, entry, oldPlaybackId)`. Admission is the existing
   replace path. On failure: restore the old identity and end the scene as `kInterrupted`,
   exactly as a failed node play does today.
5. **Re-engage policy, additively, with explicit ownership.** The ledger's one-entry-per-kind
   rule (`RecordMechanism`) only dedups the *kind*; the per-mechanism state vectors are what
   actually get restored. The contract per mechanism:
   - **Apparel / held items**: ownership is per `(actor, item)`. The first snapshot of an actor
     wins and is never recaptured (a second snapshot would record the already-hidden state and
     restore to nude). On handoff, `HideDefaultApparel` diffs the new scene's required slots
     against what is already hidden for that actor and appends an **incremental** snapshot of
     only the newly hidden items. Final restore replays all snapshots, newest first. Nothing is
     restored early by the runtime.
   - **Role equip items**: per `(actor, item)`; new items equip and append, old ones stay until
     the end. An item required by both scenes is recorded once.
   - **Camera**: re-run `RunCamera` if the posture differs; `cameraOverridden` carries over.
   - **Player input lock, fade**: keep the ledger entry; engage again only if an authored exit
     action released it.
   - **Scene controls**: see step 6.
   The ledger order stays the first engage order, so the final reverse undo is still correct.
6. **Replace the input grant, do not layer it.** The input hook copies the grant and posts the
   verb to the main thread (`InputService.cpp:139-175`); the handler only checks that the handle
   is live. Because the handle survives a handoff, a queued `kEnd` from the old scene could stop
   a replacement scene that forbids End. So:
   - add `Grant::revision`; `EngageDefaultSceneControls` increments it on every engage and the
     verb handler drops any posted verb whose grant revision is not current;
   - when the new scene **disables** controls, release the grant explicitly
     (`InputService::Release(handle)` and undo `kInputChannel`). Today's engager simply returns
     when controls are disabled (`SceneRuntime_Graph.cpp` around line 572), which would leave the
     old grant live.
7. **Events.** Fire `SCENE_HANDOFF` (new event bit `0x40`; `0x01`-`0x20` are taken) with
   `sceneId` = new id, `node` = entry, `edge` = old scene id, then `NODE_ENTER` for the entry
   node. No `SCENE_END` / `SCENE_BEGIN`. `EndTransition`.

Anchor change in `ReplaceOptions` is allowed and goes through the normal start teleport (visible
move). Getting actors to a different anchor gracefully is a higher-layer concern.

### Natives (additive)

```papyrus
bool   OSF.ReplaceScene(int aiScene, string asSceneId, OSFTypes:SceneOptions akOpts)            ; same cast, by id
bool   OSF.ReplaceSceneRoles(int aiScene, string asSceneId, string[] asRoleNames)               ; same cast, rebound
bool   OSF.ReplaceSceneByTagsQuery(int aiScene, string[] allOf, string[] anyOf, string[] noneOf) ; matchmade, same cast
string OSF.GetSceneId(int aiScene)
```

`ReplaceSceneByTagsQuery` reuses the matchmaker with the slot's roster as the cast and excludes
the current scene id, which is "change animation" in one call. `SceneOptions` fields that make no
sense mid-scene (`FadeMode`, `InPlaceMode`, `LockPlayerMode`) are ignored and documented as such.

Engine services table: `ReplaceScene` / `ReplaceSceneRoles` vmethods appended.

### Out of scope

- Cross-scene edges in content (`edges[].to` targeting another scene). Handoff is an API verb;
  authored cross-scene flow can be added later as sugar over it.
- Travel between anchors, bed prompts, eligibility.
- Carrying per-node alignment deltas across a handoff (they are keyed by scene, so they naturally
  reload for the new scene on entry).

### Acceptance

- Two standing two-role scenes with matching entry placements and different clips:
  `ReplaceScene` mid-stage produces one pose blend with no control flicker and no apparel flash.
  `SCENE_HANDOFF` then `NODE_ENTER` arrive; no `SCENE_END` until the final stop, which carries
  `cause`.
- Same pair with different entry placements: positions snap (accepted), pose still blends.
- `ReplaceSceneRoles` with the two names swapped exchanges the actors' roles; the final stop
  returns each actor to its own pre-scene transform.
- Old scene hides all apparel, new scene hides only the upper body: nothing is restored at the
  handoff; the final stop restores everything once.
- Old scene's exit lane has `osf.control.release`: the player is re-locked within the same
  frame by the new scene's default policy; no free-move frame is observable.
- Old scene grants End, new scene is `locked: true`: pressing End in the frame of the handoff
  does not stop the new scene. New scene has `sceneControls: false`: Space does nothing after the
  handoff.
- `ReplaceScene` into a furniture-anchored scene from a slot with no anchor ref, or with a ref
  the new scene rejects, is refused with a logged reason and the original scene keeps playing.
  With an accepted ref, the new scene's own `anchorOffset` is applied.
- `xmake test`: role-permutation validation; incremental apparel snapshot merge and final
  restore order as pure functions; grant-revision drop rule.

---

## Order of work

1. §0 anchor flag split, freeze, actor-keyed baseline. Small, de-risks everything after it.
2. §1 speed. Isolated; removes the browser workaround.
3. §2 event fields. Tiny; lets any consumer start depending on cause.
4. §2b handshake negotiation, shipped alone at minor `+1`, verified against the released
   browser DLL before any vmethod is appended.
5. §3 alignment. Settings keys first (also fixes the keymap TODO), then persistence, then verbs.
6. §4 handoff. Largest; lands last because it reuses §0, §1, §2 and §2b. Slot anchor ref and
   the grant revision are its first two commits, since both are also small correctness fixes on
   their own.

Each lands as its own commit set with API.md, SCENE_SCHEMA.md (align capability), UPGRADING.md
(speed behavior change) and CHANGELOG entries. API minor bump once per shipped slice.

## Open questions

- Alignment key defaults collide with vanilla numpad binds only if the player rebound them; the
  input hook does not consume keys. Acceptable for v1, or should `align` verbs consume?
- Should §3 deltas be keyed per segment for inline-staged nodes? Deferred until a real scene
  needs it.
- Should `ReplaceScene` accept a different participant set (add or drop an actor)? No for v1;
  it would require a new slot and would not be a handoff.
- Placement interpolation across node transitions (and therefore handoffs) is the one visible
  quality gap this doc accepts. It is a GraphManager change, independent of everything here, and
  should be scoped once §4 is in and the snap can be judged with real content.
- Same-frame equipment restore followed by re-hide (authored exit action plus new scene policy)
  is assumed to coalesce before the next 3D update. Unverified; check in-game during §4.
