# OSF APIs

Animation **2.0.0** uses scene ABI **2.0** and engine-services ABI **4.0**. Rebuild consumers and
update their scripts/content together. Earlier majors and short pre-v2 option structs are rejected.
New fields/methods may append within these majors; callers check version and table/struct size.
The [persistence C ABI](PERSISTENCE_API.md) is unchanged.

A node's global ID is its only registry address. `StartScene` starts that node; its stage option
selects a zero-based playback segment inside the node. `GetNode(handle)` returns the current global
ID. A live scene handle owns the cast, preparation, playback and cleanup across node transitions.
See [node catalog](NODE_CATALOG.md), [schema](SCENE_SCHEMA.md), and [migration](V2_MIGRATION.md).

Starts and transitions use [shared preparation](PREPARATION.md). A nonzero start handle reserves
actors and queues admission; true from a transition means accepted, not committed. Subscribe first.
A queued start may end without begin/enter. Manual refusal before mutation can preserve the source;
a consumed automatic boundary or a failure after application interrupts conservatively.

Authored edges and native `ReplaceNode(handle, nodeId, roles, count, options)` use one executor.
`OSFTransitionOptions` selects a stage, optional anchor/heading, and `covered` (default false).
Covered boundaries acquire exclusive fade ownership, have a bounded deadline, and reveal after
fully stamped destination poses. `ReadScenePreparation` reports refusal and reveal status.
Papyrus `ReplaceScene`, `ReplaceSceneRoles` and query helpers adapt into this same path.
Papyrus replacement reads only `Anchor`, `HeadingDeg` and `Stage` from `SceneOptions`.

Role names map current participants to destination roles; empty names preserve positional order.
Cast size, filters, placement and furniture anchor requirements must pass admission. Follow-actor
and anchor-and-pin placement cannot mix within a session. Exact requests use current registry data;
authored edges keep the source's immutable publication. No route search or node splice is added.

Transitions retain speed/pause, loop scale, caller policy/camera overrides, actor restore baselines
and owned mechanisms. Destination defaults govern inherited policy. Authored exit effects precede
application; destination entry effects follow commit. Ownership conflicts refuse preparation.
Successful order is old `NODE_EXIT`, `NODE_TRANSITION` (bit 64; `node` new ID, `edge` previous ID),
then new `NODE_ENTER`; the handle emits no second `SCENE_BEGIN`.

Events carry the global `node`, with no separate `sceneId`. `SCENE_END.cause` is completed (0),
cancelled (1), or interrupted (2); other events use -1. World replacement deliberately emits no
old-world callbacks. Native events are borrowed synchronously; Papyrus events are copied and async.

Speed is session-owned and retained across transitions. Values clamp to 0..100; zero pauses,
negative engine-service requests are refused, and invalid queries return -1. Timers/loops use
scaled playback time. Paused pose blending may still progress during a covered transition.

Alignment deltas use global node ID plus case-insensitive role in
`Documents/My Games/Starfield/SFSE/OSF/alignment-overrides.json`, **formatVersion 2**. Old-format
files are preserved and rejected; migrate their `(scene,node)` keys before saving. Anonymous roles
use `#0`, `#1`, etc., with extra prefixes to avoid named-role collisions. Set/Get use X/Y/Z and
heading degrees; Save stores the current node's roles, Reset clears a delta. Live edits require
anchored playback outside a transition, do not move the capsule, and persist through revisits in
the same publication. Unsupported/unreadable files leave the previous table intact and block saves.

Default alignment controls: numpad 5 selects a participant; 6/4, 8/2, 3/1 change X/Y/Z; 9/7 rotate;
divide changes step size, decimal resets and multiply saves. Commands require a current scene-control
grant; queued commands from a retired grant are dropped. The input hook still passes keys to the game.

Animation remains content-neutral. Consumers own semantic meaning, ranking and gameplay policy;
Animation owns timing, placement, blending, events, mechanisms and cleanup.

## Readiness

```papyrus
If OSF.IsReady()                  ; hooks installed + verified
    ...
EndIf
String v = OSF.GetVersion()         ; semver "major.minor.patch"
```

## Minimum supported OSF Animation version

For a fixed dependency, ship one manifest anywhere below `Data/OSF` whose name
ends in `.requirements.json`:

```json
{
  "schema": 1,
  "consumer": {
    "id": "ozooma10.suit-protocol",
    "name": "Suit Protocol"
  },
  "requires": {
    "osf.animation": "2.0.0"
  }
}
```

`consumer.id` is the stable machine identity used for deduplication and may
contain letters, numbers, `.`, `_`, and `-`; `consumer.name` is shown to the
player. The version must be an exact `major.minor.patch` value. OSF scans these
manifests at startup, combines duplicate IDs by taking the highest declared
minimum, reports malformed or duplicate declarations in the log, and feeds
unmet requirements into the same System Health issue and aggregated Upgrade
prompt as the runtime APIs below.

Use one manifest per consumer rather than repeating the requirement in every
content file. A consumer may ship no scenes or many content files, and its OSF
dependency should still have one source of truth. Manifest edits take effect on
the next game launch.

Keep the native helper when supporting OSF releases older than the manifest
scanner: an older host cannot discover a new data-file convention, while the
copyable native header can inspect that host's plugin metadata and warn the
player. On a current host, a manifest's display name is aliased to its stable ID,
so reporting the same consumer through both paths still produces one issue and
one prompt. The runtime API also remains appropriate for conditional
integrations.

A consumer can report the oldest OSF Animation release it supports. Call this once
during `kPostDataLoad`, before requesting the scene API:

```cpp
using OSF::API::MinimumVersionResult;

switch (OSF::API::ReportMinimumVersion("Suit Protocol", 2, 0, 0)) {
case MinimumVersionResult::kSupported:
    // Safe to request and use the OSF features this consumer needs.
    break;
case MinimumVersionResult::kUpgradeRequired:
    // OSF has already shown the player an Upgrade warning; leave this feature off.
    return;
case MinimumVersionResult::kUnavailable:
case MinimumVersionResult::kInvalidRequest:
    // Missing OSF / malformed request remains the consumer's own dependency error.
    return;
}
```

The helper resolves the standalone `OSF_ReportMinimumVersion` export; it does not
depend on scene-API readiness or change the scene ABI. A current host deduplicates
reports by consumer, retains the highest requested version, writes a warning to
the log, and publishes a durable `compat.needs-newer-osf-animation` System Health
issue containing the installed and required versions. Requirements reported
during plugin load are buffered and shown once the HUD exists. Several early
reports produce one summary warning.

The copyable header also handles an installed OSF build that predates the report
export: it reads that DLL's standard `SFSEPlugin_Version` metadata and emits the
Upgrade HUD warning from the consumer. This is why native consumers should use
this helper instead of requesting a too-new scene ABI and treating `nullptr` as
an unexplained failure.

Papyrus consumers have the matching call:

```papyrus
If !OSF.ReportMinimumVersion("My Mod", 2, 0, 0)
    Return ; warning already raised when an upgrade is required
EndIf
```

The Papyrus native necessarily exists only from the release that introduces this
reporter onward; it is a baseline for declaring requirements on later releases.
The native header fallback is the path that can warn against pre-reporter builds.

## Handles

Every `Start*` returns an opaque **scene handle** (`Int`): `0` = failed (bad id, no match, or an
actor already in a live scene — one live scene per actor is enforced). A nonzero handle drives
navigation, callbacks, cancellation, participant lookup, and stage getters. It addresses the
domain scene, not its internal playback session. When the scene terminates, the handle goes invalid
except for the short roster-survival window described under callbacks.

## Native C++ scene API

SFSE plugins can copy [src/API/OSFSceneAPI.h](../src/API/OSFSceneAPI.h) into their own source tree and
link nothing. `OSF::API::RequestSceneAPI()` resolves `OSF Animation.dll` at runtime and requests the
current ABI, **2.0**. Callback, query and node-addressing operations are all part of this baseline.
An incompatible host returns `nullptr`; do not call through an old vtable.

```cpp
void OnSceneEvent(const OSF::API::OSFSceneEvent* event, void* context)
{
    if (!event || event->size < sizeof(OSF::API::OSFSceneEvent)) {
        return;
    }
    if (event->eventType == OSF::API::SceneEventType::kCue) {
        static_cast<MyPlugin*>(context)->OnCue(
            event->sceneHandle, event->cue ? event->cue : "");
    }
}

auto* osf = OSF::API::RequestSceneAPI();
auto token = osf ? osf->RegisterSceneEventCallback(
    &OnSceneEvent, this, 0, OSF::API::SceneEventType::kCue) : 0;
// Later, if this consumer unloads its callback target:
if (token != 0) {
    osf->UnregisterSceneEventCallback(token);
}
```

Registration and unregistration are thread-safe. Unregistration is quiescent: when it returns,
callbacks executing on other threads have finished. Self-unregistration does not wait on the
current invocation itself, but it does wait for concurrent invocations on other threads.
For authored scenes the callback runs **synchronously on the game thread at the runtime mark**, after earlier timed lanes at that mark
(action → camera → sound) and before OSF queues the equivalent Papyrus event. The payload is a
borrowed immutable view: the struct and all string pointers are valid only until the callback
returns. Copy anything you retain, do not throw across the DLL boundary, and keep the callback
short. A callback may unregister itself.

Native registration tokens are process-lifetime and survive world replacement; OSF does not emit
teardown events for the discarded world. Scene filters and event masks use the same handle and bit
values documented in the Papyrus callback section below. Passing event mask `0` means all events.

### Matchmaking by query

`FindSceneByQuery` and `StartSceneByQuery` match like the Papyrus tag starts, with two additions:
**per-actor role constraints** and an explicit anchor mode. Both run on the game thread.

```cpp
const char* allOf[] = { "adult", "mf" };
const char* roles[] = { "top", "" };            // actor 0 must take the "top" role; actor 1 any role
OSF::API::OSFSceneQuery query;
query.allOf = allOf;  query.allOfCount = 2;
query.roles = roles;  query.roleCount = 2;      // 0 or exactly the actor count
query.anchor = bed;   query.requireAnchor = 1;  // only scenes whose anchor accepts this ref

OSF::API::OSFSceneMatch match;
if (osf->FindSceneByQuery(actors, 2, query, match)) {
    // match.nodeId, and for each caller actor i: match.roleIndex[i] and match.roles[i]
}
OSF::API::OSFStartOptions opts;
std::int32_t handle = osf->StartSceneByQuery(actors, 2, query, opts);
```

- A role constraint is hard. Actor `i` binds only to a role with that name, compared
  case-insensitively, and still must pass the role's gender, keyword and race filters. An empty or
  null entry accepts any role. Among valid assignments, cast order breaks ties.
- `requireAnchor = 1` matches only anchor-bound scenes the anchor accepts. Otherwise free scenes
  match too, and anchor-bound scenes need an accepting anchor.
- `FindSceneByQuery` has no side effects and supports at most `kOSFSceneMatchMaxActors` (8) actors.
  It picks by priority tier and weighted random, so two calls can differ. To start exactly the
  found scene, order the actors by `roleIndex` and call `StartScene` with the same anchor in
  `OSFStartOptions.anchorRef`.
- `StartSceneByQuery` always enters at the authored entry; `startStage` is ignored. A non-null
  `OSFStartOptions.anchorRef` that differs from `query.anchor` is rejected. It shows no HUD messages.
- Both structs are size-prefixed and append-only. Malformed input returns false or 0 and logs why.

## Starting scenes

There are **three public ways to start a registered scene** from the content registry's scene
namespace. Each takes an optional trailing `SceneOptions` struct (`None` = all defaults) for world
anchoring and per-start policy overrides, so the common case stays a one-liner.

```papyrus
Actor[] actors = new Actor[2]
actors[0] = akA
actors[1] = akB

; By id (a single registry lookup — one scene namespace, no prefixes, no fallback):
int h = OSF.StartScene(actors, "author.scenes.demo")

; By matchmaking — tags + role/gender/keyword/race fit across the content registry's scene namespace,
; chosen by priority tier then weighted-random:
string[] tags = new string[1]
tags[0] = "takedown"
int h2 = OSF.StartSceneByTags(actors, tags)

; Boolean query form (allOf / anyOf / noneOf — pass real empty arrays, never None):
int h3 = OSF.StartSceneByTagsQuery(actors, allOf, anyOf, noneOf)

; Query form with role constraints: roles[i] restricts actors[i] to that role name ("" = any).
string[] roles = new string[2]
roles[0] = "top"
int h4 = OSF.StartSceneByQuery(actors, allOf, anyOf, noneOf, roles)
string id = OSF.FindSceneByQuery(actors, allOf, anyOf, noneOf, roles)   ; match only, no start
```

`StartSceneByQuery` and `FindSceneByQuery` match like `StartSceneByTagsQuery` and add per-actor role
constraints. A constraint is hard and still requires the role's filters to pass; pass `new string[0]`
for none, or one entry per actor. `abRequireAnchor = true` limits the match to anchor-bound scenes that
the anchor accepts (`SceneOptions.Anchor` for Start, `akAnchor` for Find). Find picks by priority and
weighted random without side effects, so a later Start may choose differently; start the found id
with `StartSceneRoles` to get exactly that scene. The native equivalents are described under
*Matchmaking by query*.

For raw one-actor clip playback outside a registered scene, use the primitive `OSF.Play(actor, file)`;
it creates a solo playback session rather than a domain scene.

### Advanced / porting API

The primary `OSF.psc` script is kept small for common “start a registered scene” consumers. Porting-oriented dynamic starts live on `OSFAdvanced.psc`:

```papyrus
int hFiles = OSFAdvanced.StartSceneFiles(actors, files, opts)
int hFilesPlaced = OSFAdvanced.StartSceneFilesPlaced(actors, files, x, y, z, headingDeg, opts)
int hRoles = OSFAdvanced.StartSceneRolesEx(actors, "author.scene", roles, opts)
int hStages = OSFAdvanced.StartSceneStages(actors, stageMajorFiles, timers, loops, blends, opts)
int hStagesPlaced = OSFAdvanced.StartSceneStagesPlaced(actors, stageMajorFiles, timers, loops, blends, x, y, z, headingDeg, opts)
bool ok = OSFAdvanced.PlaySequence(actor, files, loops, blends, false)
bool hidden = OSFAdvanced.HideEquipment(hFiles, actors[0], -1)
bool restored = OSFAdvanced.RestoreEquipment(hFiles)
int stopped = OSFAdvanced.StopAllForActors(actors)
string[] loadProblems = OSFAdvanced.GetSceneLoadErrors()
string[] missingClips = OSFAdvanced.GetMissingClipRefs()
```

`GetSceneLoadErrors` is an established compatibility name. Its result is the content registry's
unified load-problem list and can include curated-animation problems as well as scene
problems.

Dynamic animation clip specs accept the same compatibility shortcuts as scene JSON:
`naf:Path.glb` resolves under `Data/NAF`, and `File.glb:AnimName` selects a named GLB animation. The
parameter name `file` is an established API spelling; callers are supplying an unresolved clip spec,
not an already-loaded file object.

`StartSceneFilesPlaced` / `StartSceneStagesPlaced` add dynamic participant placement offsets without
JSON. `x`/`y`/`z` are local offsets relative to the scene's world anchor, and `headingDeg` is relative
facing in degrees. Each placement array may be empty, which means zero for that component. For
`StartSceneFilesPlaced`, non-empty placement arrays must be actor-count length. For
`StartSceneStagesPlaced`, non-empty placement arrays may be actor-count length (reused for every
linear stage) or stage-major length matching `stageMajorFiles`.

`HideEquipment(scene, actor, slotMask)` is a ledger-safe ad-hoc **hide-apparel** helper for dynamic
starts. The actor must be a participant in `scene`; `slotMask` uses Starfield ARMO biped slot bits,
`-1` hides all apparel, and `0` hides nothing. Hidden items are restored automatically when the scene
terminates, or early with `RestoreEquipment(scene)`, subject to current inventory and surviving
owner claims. Equipment/preserve claims are exempt from this optional hiding.

`SceneOptions` carries the optional modifiers (set only the fields you need; each `Start*` reads only
the ones that apply to it). Its stable Papyrus field names intentionally differ from several preferred
JSON keys:

| Field | Type | Applies to | Meaning |
|---|---|---|---|
| `Anchor` | `ObjectReference` | option-bearing registered and advanced starts | use a world anchor at a ref (furniture/bed/marker) instead of co-locating at `actors[0]` |
| `HeadingDeg` | `Float` | (with `Anchor`) | world-anchor facing in degrees; `< 0` = the ref's own heading |
| `HideApparelMode` | `Int` | all option-bearing starts | override for JSON `hideApparel`: override with `OSF.INHERIT()`/`OFF()`/`ON()` |
| `PlayerInputLockMode` | `Int` | all option-bearing starts | override for JSON `playerInputLock`: override with `OSF.INHERIT()`/`OFF()`/`ON()` |
| `SceneControlsMode` | `Int` | all option-bearing starts | override for JSON `sceneControls`: inherit, revoke, or grant the scene-control channel wholesale |
| `FadeMode` | `Int` | all option-bearing starts | override the optional start fade-to-black curtain: `OSF.INHERIT()`/`OFF()`/`ON()` |
| `FollowActorMode` | `Int` | all option-bearing starts | override for JSON `placement`: `ON()` = `followActor`, `OFF()` = `anchorAndPin`, `INHERIT()` = authored default |
| `Camera` | `String` | all option-bearing starts | override the authored camera posture; empty string inherits it |
| `LoopScale` | `Float` | all option-bearing starts | multiply every loop-driven stage's loop count (`1.0` = unchanged); see below |
| `Stage` | `Int` | StartScene, StartSceneRolesEx, StartSceneStages(Placed) | zero-based initial segment in the selected node or ad-hoc plan; `0` selects the first segment |
| `Speed` | `Float` | all option-bearing starts | seed the scene-owned playback speed before the first node; persists across transitions |
| `BlendIn` | `Float` | StartSceneFiles(Placed), StartSceneStages(Placed) | default blend-in for ad-hoc plans; registered definitions keep their authored blend |

`SceneOptions` holds only scalar/ref fields — **Papyrus structs can't have array members**, so named-role
binding stays in separate functions. `StartSceneRoles` is the compact compatibility form without
options; `StartSceneRolesEx` accepts the same binding plus `SceneOptions`.

Registered-scene launch compatibility differs between the two APIs:

| Start path | Out-of-range stage | Speed |
|---|---|---|
| Papyrus by ID or `StartSceneRolesEx` | Refuses the start | Seeds `SceneOptions.Speed` before playback |
| Native `StartScene` / `StartSceneRoles` | Falls back to segment zero | Seeds `OSFStartOptions.speed` before playback |
| Papyrus matchmaking (`StartSceneByTags`, query, or anchor) | Uses segment zero | Seeds `SceneOptions.Speed` before playback |

For registered starts, stage `0` selects the first segment of the named node. Furniture-bound scenes require an accepted reference anchor even when the native
caller supplies explicit world coordinates.

**Per-start tri-state overrides (`HideApparelMode` / `PlayerInputLockMode` / `SceneControlsMode` / `FadeMode` /
`FollowActorMode`).** Write these fields with
the `OSF.INHERIT()` (= -1, leave the scene/file default), `OSF.OFF()` (= 0, force off), and `OSF.ON()`
(= 1, force on) helpers — **not** bare `0`/`1`, because `0` means *force off*, not "leave default". An unset
field (default `-1`) inherits the scene's authored value. For `FollowActorMode`, "off" means canonical
anchor-and-pin placement rather than disabling placement. Disabling apparel hiding is undo-safe
(nothing recorded → nothing restored).

**`LoopScale`.** Multiplies the loop count of every *loop-driven* stage (`new = max(1, round(loops × scale))`),
so a terminal-driven `GlobalVariable` can lengthen/shorten scenes. It affects only stages that already loop a
fixed number of times; "loop until advance" (hold) and timer-only stages are untouched, so on a mixed graph
the felt effect is uneven. Sanitized: `≤ 0` / NaN → `1.0` (no scaling); clamped to a ceiling so a runaway
value can't mint a stage that never auto-advances. Re-applied on every flow-node entry (never compounded).
Scene JSON can also [read loop counts directly from a game global](SCENE_SCHEMA.md)
using `"loops": { "global": "Plugin.esm|0x1234", "default": 3 }` (1.5.0).
Those counts are captured at each node admission, then multiplied by `LoopScale`; zero remains a hold.
The final scaled count saturates at the signed 32-bit maximum.

> **Capability note:** the DLL and a consumer's compiled scripts ship independently. A consumer compiled
> against a newer `OSFTypes` but running an older DLL will have these fields silently ignored (the old native
> doesn't read them). Gate on `OSF.GetVersion()` if you need to know the override fields are honored.

```papyrus
; World-anchored at a ref (the old StartSceneByTagsAt / StartSceneAt):
OSFTypes:SceneOptions opts = new OSFTypes:SceneOptions
opts.Anchor = akBed                                           ; opts.HeadingDeg stays -1.0 = bed's heading
int h5 = OSF.StartScene(actors, "author.scenes.demo", opts)   ; by id, anchor-and-pin placement
int h6 = OSF.StartSceneByTags(actors, tags, opts)             ; matchmade, anchor-and-pin placement

; Bind actors to named roles (its own function — the role array can't live in a struct):
int h7 = OSF.StartSceneRoles(actors, "author.scenes.demo", roleNames)
```

> **None-array footgun:** passing a `None` array into a native that expects an array can crash. Always
> pass a real (possibly empty) array — `new String[0]` is valid and accepted. Arrays you *receive* from
> OSF are always real. (A `None` *struct* is fine — that's how an omitted `SceneOptions` arrives.)

## Stopping & state

```papyrus
OSF.StopScene(h)                 ; by handle (fires SCENE_END + runs the undo ledger)
OSF.StopSceneForActor(akActor)   ; by actor; false if it's in no scene
Bool playing = OSF.IsPlaying(akActor)
Actor[] roster = OSF.GetSceneParticipants(h)
```

`StopScene*` requests **cancellation**. A scene may instead **complete** at its authored natural end or
be **interrupted** by external runtime state. All are termination causes, and every termination path
runs the **undo ledger**, which reverses the mechanisms the scene engaged
(control/camera/weapon/equipment/fade) in reverse order. Consumers do not clean up engaged mechanisms
by hand.

## Navigation (def-backed scenes)

```papyrus
Bool moved = OSF.AdvanceScene(h)               ; take the current flow node's default scene edge
Bool went  = OSF.NavigateScene(h, "finish")    ; take a named branchable scene edge
int n = OSF.GetSceneEdgeCount(h)               ; branchable scene edges (for building a menu)
String eid = OSF.GetSceneEdgeId(h, 0)
String lbl = OSF.GetSceneEdgeLabel(h, 0)
```

`GetSceneStage`/`SetSceneStage` and their actor variants address the current node's zero-based
playback segment. They do not navigate between nodes. `AdvanceScene` advances an internal segment
first, then takes the node's default edge or ends it. `NavigateScene` selects a named edge directly.

## Papyrus scene-event callbacks

Register a receiver to get `OSFTypes:SceneEvent` structs (see
[dist/Scripts/Source/OSFTypes.psc](../dist/Scripts/Source/OSFTypes.psc)). Dispatch is **asynchronous**
— the payload is a snapshot struct (no dispatch-time getters).

Cues authored with `delivery:"native"` reach synchronous native callbacks only; they skip
Papyrus target collection, payload construction, and VM dispatch. Omitted delivery or
`delivery:"both"` keeps existing behavior. This per-cue option does not change scene/node
lifecycle events, role binding, source timestamps, or `trigger:<id>` navigation. See
[cue delivery](SCENE_SCHEMA.md#cue-delivery).

> **Participants at scene end:** the one exception to "no live getters" — the event's
> `sceneHandle` stays *roster-queryable* through the `EVENT_SCENE_END` callback, so
> `OSF.GetSceneParticipants(akEvent.sceneHandle)` returns who took part even though the scene has
> ended. Retired rosters use a bounded recent-history window (currently 256), so copy the roster
> during the callback rather than caching the handle indefinitely. `SCENE_END` carries no
> `actorRef` itself, so this is how an end handler enumerates participants. Note `SCENE_END` fires on
> runtime termination paths that dispatch callbacks, including natural completion and `Stop()`
> cancellation. `cause` distinguishes completion (0), cancellation (1), and interruption (2).
> World-replacing load teardown clears the VM relay and native handle table without dispatching
> callbacks into the discarded world. Native C++
> registrations remain registered, but receive no teardown event.

```papyrus
; aiScene 0 = any scene; aiEventMask is a bitmask of OSF.EVENT_*().
; Instance form — handler is an instance method on the receiver ScriptObject:
int token = OSF.RegisterSceneCallback(Self, "OnSceneEvent", 0, OSF.EVENT_ALL())

; Static form — register a GLOBAL function on a named script (no instance needed); the handler
; must be `Function OnSceneEvent(OSFTypes:SceneEvent akEvent) Global` on the script `asScript`:
int token2 = OSF.RegisterSceneCallbackStatic("MyQuestScript", "OnSceneEvent", 0, OSF.EVENT_ALL())

OSF.UnregisterSceneCallback(token)   ; same unregister for either form

Function OnSceneEvent(OSFTypes:SceneEvent akEvent)
    If akEvent.eventType == OSF.EVENT_SCENE_END()
        Actor a = akEvent.actorRef
        ; akEvent fields: sceneHandle, eventType, node, edge, cue, actionType,
        ;                 actorRef, role, loopIndex, time, anchor, result, cause
        ; `node`/`edge` identify a flow node and scene edge. `anchor` is the legacy
        ; ABI name for a named track position, not a world anchor.
    EndIf
EndFunction
```

**Event-type bits** (compose into the mask; exposed as functions so they read on the type, `OSF.X()`):

| Function | Bit | Fires when |
|----------|----:|------------|
| `OSF.EVENT_NODE_ENTER()` | 1 | a flow node is entered (also dispatches that flow node's enter cues) |
| `OSF.EVENT_NODE_EXIT()` | 2 | a flow node is exited |
| `OSF.EVENT_CUE()` | 4 | a `cue` track entry fires |
| `OSF.EVENT_ACTION()` | 8 | a custom (non-`osf.*`) action fires |
| `OSF.EVENT_SCENE_END()` | 16 | the scene terminated on a callback-emitting path; `cause` identifies why |
| `OSF.EVENT_SCENE_BEGIN()` | 32 | the scene started (fires once, before the entry flow node's `NODE_ENTER`) |
| `OSF.EVENT_NODE_TRANSITION()` | 64 | the handle changed node; `node` is new, `edge` is the previous global ID |
| `OSF.EVENT_ALL()` | 65535 | every type |

`SCENE_BEGIN` is the lifecycle-open bookend of `SCENE_END`: it fires exactly once per scene as the
first event, after OSF has applied start setup (player input lock, hide apparel, role equip, optional
fade, scene controls) and the entry flow node's animation is playing — so the scene is fully live.
Its `node` field carries the entry flow node; like `SCENE_END` it carries no `actorRef`. The handle is live when
`SCENE_BEGIN` is dispatched; because Papyrus dispatch is async (the callback runs on a later VM tick), it
uses the same bounded recent-roster window as `SCENE_END`. Read/copy participants promptly; a getter
may return empty for a very short (`once`/0-duration) scene that ended before the queued callback ran,
or after an extreme burst has expired that handle from the recent-history window.

`OSF.RESULT_OK()` / `RESULT_BAD_ROLE()` / `RESULT_RUNTIME_FAILURE()` / `RESULT_NO_HANDLER()` decode
`akEvent.result`.

## Automatic progression controls

For a complete Papyrus query-launch, alignment, role-swap and cleanup example,
see [query and handoff](examples/query-handoff/README.md).

Engine services 4.0 includes `SetSceneAutoAdvance(handle, enabled)` and `ReadSceneControls`.
Both require the verified frame drain. Automatic progression defaults true and survives transitions.
False holds all automatic node/stage boundaries while sampling and cues continue. There is no content
grant. Explicit navigation/stop work. Re-enabling resets timer/loop budgets at the current phase,
without replaying once-only cues; a completed playback cannot be revived.

`ReadSceneControls` returns copied JSON with `node`, `stage`, `automatic`, `speed`, and available
named `edges` (`id`, `label`). It returns false for an invalid handle. Recheck state when acting.

## Owned audio channels

Engine services 4.0 provides three any-thread operations:

| Operation | Contract |
|---|---|
| `CreateAudioChannel()` | Returns an opaque process-local handle, or zero when the 4096 live-channel limit is reached. Handles are never reused. |
| `PlayAudioChannel(channel, spec)` | Copies a literal Data-relative file path or `event:` spec. File preparation runs on the bounded audio workers; true means queued, not decoded or audible. Events submit inline. Unsupported paths, pool queries and retired handles return false. |
| `CloseAudioChannel(channel)` | Permanently retires the handle, invalidates pending requests and stops its submitted voice. Repeated close is harmless and returns false. Other channels and legacy authored slots are unaffected. |

Each channel holds one published voice. New publication stops its previous voice; an older
completion cannot submit after a newer request has published. A newer queued request that fails
to prepare does not replace an existing voice. There is no automatic backlog/coalescing policy.

Media preparation occurs outside the service lock. The final validity check, Wwise submission
and publication share the same lock as channel close and world teardown. If close wins, an old
job cannot submit. If submission wins, close waits for that short commit and stops its playing
ID before returning. No decoder/file IO or consumer callback runs inside the commit. This is a
submission-order guarantee, not proof of silence or a bound on output-device latency.

The caller owns scope policy: allocate separate channels for effects that may overlap, close
them at the appropriate lifecycle boundary, and create fresh handles for resumed scopes.
Animation does not infer scene, node, actor or provider identity from a channel. In native scene
callbacks, `NODE_ENTER` is delivered before authored entry sounds, allowing a consumer to close
generated voice while retaining a separate contact channel. World replacement automatically
revokes every channel; consumers must forget the old handles. Nothing is persisted.

Playback currently uses the listener emitter and existing Wwise dialogue bus. This API does not
promise actor-positioned emitters, separate mixer buses, pool selection, subtitles or prewarming.

Services 4.0 provides `BindActorSoundChannel(actor, channel)` for consumers that must give authored
sound lanes/actions and generated playback the same lifetime. Call in game context before entry
sounds (native `NODE_ENTER` runs first). The live owned channel receives subsequent sounds for
that actor after normal pool resolution. Another live binding cannot be stolen. Closing the
channel stops its current sound, cancels its queued work and removes the binding; world reset
also clears bindings. Actorless sounds and other actors retain their existing behavior. Binding
opts all role-authored sounds for that actor into this scope; use separate owned channels for
independent contact/effects. The caller still owns and closes the channel. No content policy is inferred.
Legacy sound lanes keep their normal scene-stop tail policy, while gaining the same final
world/sequence check before submission. Consumers should call the service directly from native
beat callbacks; posting another frame command would introduce an unnecessary queue hop.

## Optional engine services

Services 4.0 includes `CreateAudioChannelWithOptions`, `AudioClockNs` and `PlayAudioChannelAt`.
Options are fixed for a channel's lifetime. `latestPending=1` retains at most one waiting job
per channel, replacing it in place before checking global queue capacity. Executing older jobs
recheck supersession before preparation and before submission. An admitted replacement that
later fails decoding does not revive discarded work; it also does not stop the playing voice.
Invalid specs and rejected admissions do not supersede pending work. Closing an opted channel
removes its waiting job immediately and fences executing work through the existing commit lock.

`maxAgeMs=0` disables expiry. A positive limit checks age before admission, before preparation,
and immediately before Wwise submission, even when there is no newer request. Use the native scene
event's `detectedNs` for timed cues; it preserves the original monotonic mark-detection time
across the native frame queue. For other requests obtain a timestamp from `AudioClockNs` before
selection work. `PlayAudioChannelAt` rejects zero/future timestamps. Plain `PlayAudioChannel`
starts age at entry. These are submission limits, not audible-onset or sound-duration limits.
Detection time is not a reconstructed animation due time. The default `CreateAudioChannel`
continues without coalescing or expiry, including its existing failed-replacement behavior.

The scene ABI is 2.0. `OSF_RequestEngineAPI` provides services ABI 4.0
for verified-frame dispatch, lifecycle subscriptions, catalog/import metadata, matching, active state,
playback controls, owned audio channels, shared camera/input mechanisms, and actor-state scopes. See [BROWSER_ADDON.md](BROWSER_ADDON.md) and
`src/API/OSFEngineAPI.h` for ownership, callback lifetimes, cancellation, and JSON contracts. Gameplay
consumers need neither OSF Animation Browser nor OSF UI.

## Shared actor-state ownership

Services 4.0 includes persistent and session scopes with equipment, preservation, and required
or optional hiding claims. Scene role equipment, automatic gear, equipment actions and
stripping use the same service. Release is selective, save suspension retains persistent
demands, and world replacement revokes runtime-only tokens. See [ACTOR_STATE.md](ACTOR_STATE.md)
for the native API, conflicts, cleanup retry, and base-form inventory provenance limits.
Services 4.0 includes read-only `ReadActorEquipment` observations from the shared H2 backend.
Services 4.0 includes per-actor ownership/equipment revisions and a bounded notification journal; see [actor state](ACTOR_STATE.md).

Services 4.0 includes [shared preparation](PREPARATION.md), including versioned preparers,
equipment plans, exact resource reservations, bounded readiness and selective rollback across
starts and all runtime transitions. Item-mesh/attachment readiness requires a consumer backend;
pose-layer composition remains separate work.

## In-game UI, hotkeys, and the wheel tag contract

The optional **OSF Animation Browser** addon provides the scene browser, animation wheel,
vanilla library, and emote pack; **OSF UI 2.0** renders the view. **OSF Settings 1.0**
owns settings and hotkeys on the **OSF Animation** page. Both hotkeys ship unbound; the browser
is also available from the OSF Settings Launcher tab. `OSF.OpenBrowser()` remains available to
Papyrus through an optional addon forwarder and returns false when Browser or OSF UI is unavailable.
The wheel is opened by the addon hotkey or native `OSFBrowser_OpenWheel`; there is no `OSF.OpenWheel`
Papyrus native in the current source. The legacy `Data/OSF/settings.json` is no longer read.

Content packs join these surfaces purely by **tags**, no code: a solo, free-space, self-terminating
scene tagged `player.emote.<name>` appears as an **emote** in the unified Browse catalog and in the
default Quick Access wheel. Older browser copy may label this playable category `Action`; that legacy
UI term is unrelated to a scene's authored `action` track. The well-known tag contract lives in
[SCENE_SCHEMA.md](SCENE_SCHEMA.md).

## Discovery & diagnostics

```papyrus
int count = OSF.ReloadPacks()  ; rescan Data/OSF/**/*.osf.json and *.sounds.json
```

`ReloadPacks()` **reloads content**: it rebuilds the content registry (scenes, curated animation
entries, and per-file import results), reloads sound pools, clears clip import caches, and returns the
loaded authored **node** count. `ReloadPacks` is the established Papyrus binding name; a
content pack is only an author/product grouping and is not the reload boundary.

## Primitives (advanced)

`Play` / `Stop` (solo clip), `SetSpeed` / `GetSpeed` (1.0 = authored, 0 = freeze),
`SetAnchor` / `ClearAnchor` (pin a solo playback session to a world point), `GetCurrentAnimation`, and node
stage getters/setters by handle or actor.

## API stability policy

Pre-1.0 (`0.x`) the surface is still settling and may change between releases. From **1.0** on, natives
are never removed or re-signatured within a major version (minor versions only **add**). The
`OSFTypes:SceneEvent` struct member set is part of the ABI — new fields append at the end, so old
callbacks keep working. The native C++ API follows the same additive rule: minor versions append
vmethods or POD fields, while a major version is required for an incompatible layout change.

Services 4.0 includes `GuardPreparationCommit`, a per-attempt final validation lease after all
Validate callbacks and final reconciliation. See [shared preparation](PREPARATION.md).

## Global node catalog

Catalog schema **4** contains one `nodes` array, each with its own metadata, roles and `stages`.
Compact provider input is schema **2**. `MatchCast` and `MatchAnchor` return arrays of global node
IDs. `ReplaceNode` is the single exact transition service. The previous scene/node pair, startNode,
MatchNode variants and covered/role-specific replacement slots are removed.

See [Node catalog and execution](NODE_CATALOG.md) for source clip offsets and publication ownership.
Offline release evidence and remaining live gaps are recorded in [v2 acceptance](RELEASE_2.0.0_ACCEPTANCE.md).
