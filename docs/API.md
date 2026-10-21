# OSF APIs

Native SFSE plugins that need per-save records should use the [shared persistence C ABI](PERSISTENCE_API.md).

Scene starts and transitions now use [shared preparation](PREPARATION.md). A nonzero start handle
reserves actors and queues admission; successful begin/enter follows preparation and playback.
A true transition return means accepted, not completed. Subscribe before starting; rejected
queued starts emit interrupted/cancelled SCENE_END without begin/enter.

Domain terms in this guide follow [OSF domain vocabulary](VOCABULARY.md). The canonical JSON aliases
documented in [SCENE_SCHEMA.md](SCENE_SCHEMA.md) do not rename existing Papyrus or native API fields;
names shown in code remain compatibility spellings.

Scene node transitions keep the world anchor resolved on first playback. An explicit anchor
also enables transform restoration on termination; a frozen implicit anchor does not. Restore
baselines are captured once per actor at scene start and survive node changes and role swaps.

`SetSceneSpeed(handle, speed)` / `GetSceneSpeed(handle)` use scene-owned speed, retained across
all node changes. Actor-based `SetSpeed` also updates this state for participants. Values clamp
to 0..100 (0 pauses); engine-services calls reject negative values, and hotkey adjustments use
0.1..5. Invalid/ended scene queries return -1. Timers and loop counts use scaled playback time,
so changing speed stretches authored timing. Pause/resume remembers speed independently per scene.

Scene events now append `sceneId` (the definition id at dispatch, empty for ad-hoc scenes) and
`cause`: SCENE_END reports `CAUSE_COMPLETED()` = 0, `CAUSE_CANCELLED()` = 1, or
`CAUSE_INTERRUPTED()` = 2; other events report -1. Native scene ABI 1.4 appends these fields
after the old 88-byte event prefix; check `size` before reading them from older hosts. Older
Papyrus consumers remain compatible. World-replacing load teardown still emits no scene events
into the discarded world.

Engine services 3.1 negotiates minor versions: requests need the same major and a minor no
newer than the host. Clients check the major, minimum minor, and the table size compiled into
their header. The released 3.0 Browser checks exact equality, so requests for 3.0 receive a
stable 3.0 version/prefix view of the current implementation. Requests for 3.1 or later receive
the full current table. Never read appended functions without checking the table size.

Scene keyboard controls use key settings under OSF Animation > Scenes - Keyboard controls.
They are dispatched only while the active scene grants their capability. Rebinding replaces
the default key, and unbinding disables it. The input hook continues to pass keys to the game.

Alignment overrides are deltas keyed by exact scene id and node id, and case-insensitive role name in
`Documents/My Games/Starfield/SFSE/OSF/alignment-overrides.json` (formatVersion 1).
Anonymous roles use their zero-based cast index (`#0`, `#1`, ...); if an authored role already
uses that key, additional `#` prefixes avoid the collision. These keys also identify anonymous
roles in Set/Get/Reset calls and the alignment HUD. Named roles keep their authored names.
The JSON heading is degrees; missing components mean zero. Zero rows are removed, with no
wildcard fallback. Reload replaces persisted rows while live scenes retain their working copies.
Invalid rows or scene/node branches are skipped with a Mod Issue; valid rows remain available
and survive later saves. Unreadable documents, unsupported versions and document size/row limits
leave the previous table intact and block saving until a successful reload; scenes still play.

Engine services 3.2 and Papyrus expose `SetSceneAlignment`, `GetSceneAlignment`,
`SaveSceneAlignment`, and `ResetSceneAlignment`. Set/Get use four floats: X, Y, Z, heading in
degrees. Set replaces the current delta; Reset zeroes it. Save persists all roles of the current
node. Unsaved deltas survive node revisits and are discarded at scene end. Invalid Papyrus Get
returns an empty array; native Get returns false. Mutations require a registered anchored scene
outside a transition. One node's delta applies to all inline segments and uses the host identity
for `use` nodes. The capsule is not moved by live nudges.

Default alignment keys: numpad 5 selects a participant; 6/4, 8/2, and 3/1 change X, Y, and Z;
9/7 rotate; divide toggles 1.0/0.1 position and 5/1 degree steps; decimal resets; multiply saves.
The HUD shows the role, current delta, step mode, and unsaved status. Numpad +/- remain speed.

`ReplaceScene(handle, id, options)`, `ReplaceSceneRoles(handle, id, roleNames)` and
`ReplaceSceneByTagsQuery(handle, allOf, anyOf, noneOf)` and
`ReplaceSceneByQuery(handle, allOf, anyOf, noneOf, roles, anchor, requireAnchor)` replace the definition for the same cast
without changing the handle. `GetSceneId(handle)` returns the live definition id, or empty when
invalid/ad-hoc. Native engine services **3.3** append `ReplaceScene` and `ReplaceSceneRoles`; **3.4**
appends `ReplaceSceneByQuery`, which takes an `OSFSceneQuery` (see *Matchmaking by query*).
Names correspond to the current participant roster: `roleNames[i]` assigns participant `i` to
a role in the new definition. Empty names preserve positional order; duplicates, missing roles,
different cast sizes and failed role filters are refused before the old node exits.

Handoff retains speed/pause, loop scale, placement, the camera override, the original actor-owned
restore baselines and mechanism ownership. Unsaved alignment is discarded and the new scene's
alignment rows load on entry. The existing replacement path blends poses; changed positions snap.
The runtime adds no fade or blanket control/equipment restore. Authored exit equipment and target
defaults/entry equipment are preflighted and applied through the shared ownership transaction.
Other exit effects precede target application; non-equipment entry effects follow playback commit.
Conflicts with another owner refuse preparation; cleanup releases only owned demands.

Papyrus replacement options read only `Anchor`, `HeadingDeg` and `Stage`; all other fields,
including `FadeMode`, `InPlaceMode`, `LockPlayerMode`, `Speed`, `Camera`, `LoopScale`, `StripMode`,
`PlayerControlMode` and `BlendIn`, are ignored. The native table reads only `anchorRef` and
`startStage`. Stage 0 uses the authored entry; an out-of-range positive linear stage is refused.
Changing an anchor visibly teleports. Anchor-bound replacements require an accepted retained or
supplied reference and compose their own offset from the saved base transform. Raw-coordinate
starts cannot satisfy an anchor requirement without a new reference. Follow-actor and anchored
scenes cannot be mixed. Matchmade replacement excludes the current id before priority selection.

Successful order is old `NODE_EXIT`, `SCENE_HANDOFF` (bit 64, `sceneId` = new id, `node` = entry,
`edge` = old id), then new `NODE_ENTER`; no new `SCENE_BEGIN` or `SCENE_END` is emitted. A playback
failure after exit restores the old identity and ends it with `CAUSE_INTERRUPTED`. Validation
failures leave the old scene playing. Each input grant has a revision: queued commands from the
old grant are dropped, and a replacement disabling scene controls releases the grant outright.

OSF exposes its surface as the global Papyrus script `OSF` (declared in
[dist/Scripts/Source/OSF.psc](../dist/Scripts/Source/OSF.psc)). Call it from any script —
`OSF.StartSceneByTags(...)`, `OSF.StopScene(...)`, etc. This doc is the integration guide; the
per-native reference is the doc-commented `OSF.psc` itself.

OSF is **content-neutral**: it provides playback and scene *mechanisms* only. A domain **scene** owns
roles, flow, policy, events, participants, and cleanup; a lower-level **playback session** owns
synchronized animation sampling. Gameplay policy — *when*,
*why*, and *with what odds* to start an interaction — belongs in your mod (the SIF framework is the
worked example; see [GETTING_STARTED.md](GETTING_STARTED.md)).

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
    "osf.animation": "1.5.0"
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

switch (OSF::API::ReportMinimumVersion("Suit Protocol", 1, 5, 0)) {
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
If !OSF.ReportMinimumVersion("My Mod", 1, 5, 0)
    Return ; warning already raised when an upgrade is required
EndIf
```

The Papyrus native necessarily exists only from the release that introduces this
reporter onward; it is a baseline for declaring requirements on later releases.
The native header fallback is the path that can warn against pre-reporter builds.

## Handles

Every `Start*` returns an opaque **scene handle** (`Int`): `0` = failed (bad id, no match, or an
actor already in a live scene — one live scene per actor is enforced). A nonzero handle drives
navigation, callbacks, cancellation, participant lookup, and linear-stage getters. It addresses the
domain scene, not its internal playback session. When the scene terminates, the handle goes invalid
except for the short roster-survival window described under callbacks.

## Native C++ scene API

SFSE plugins can copy [src/API/OSFSceneAPI.h](../src/API/OSFSceneAPI.h) into their own source tree and
link nothing. `OSF::API::RequestSceneAPI()` resolves `OSF Animation.dll` at runtime and requests the
current ABI. ABI 1.3 appends synchronous native scene-event registration to the existing start,
stop, navigation, playback, and query surface. A consumer that requires callbacks should request
1.3 or later. ABI 1.4 adds event identity/cause, and ABI 1.5 adds matchmaking by query; the current
header defaults to 1.5. An older OSF build returns `nullptr` for a newer request instead of exposing
unsupported fields or a short vtable.

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

### Matchmaking by query (ABI 1.5)

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
    // match.sceneId, and for each caller actor i: match.roleIndex[i] and match.roles[i]
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
| `StripMode` | `Int` | all option-bearing starts | compatibility field for JSON `hideApparel`: override with `OSF.INHERIT()`/`OFF()`/`ON()` |
| `LockPlayerMode` | `Int` | all option-bearing starts | compatibility field for JSON `playerInputLock`: override with `OSF.INHERIT()`/`OFF()`/`ON()` |
| `PlayerControlMode` | `Int` | all option-bearing starts | compatibility field for JSON `sceneControls`: inherit, revoke, or grant the scene-control channel wholesale |
| `FadeMode` | `Int` | all option-bearing starts | override the optional start fade-to-black curtain: `OSF.INHERIT()`/`OFF()`/`ON()` |
| `InPlaceMode` | `Int` | all option-bearing starts | compatibility field for JSON `placement`: `ON()` = `followActor`, `OFF()` = `anchorAndPin`, `INHERIT()` = authored default |
| `Camera` | `String` | all option-bearing starts | override the authored camera posture; empty string inherits it |
| `LoopScale` | `Float` | all option-bearing starts | multiply every loop-driven stage's loop count (`1.0` = unchanged); see below |
| `Stage` | `Int` | StartScene, StartSceneRolesEx, StartSceneStages(Placed) | registered scenes enter `linearStages[index]` before any playback begins; ad-hoc staged starts select the initial playback segment. `0` uses the normal entry/first segment |
| `Speed` | `Float` | all option-bearing starts | seed the scene-owned playback speed before the first node; persists across transitions |
| `BlendIn` | `Float` | StartSceneFiles(Placed), StartSceneStages(Placed) | default blend-in for ad-hoc plans; registered definitions keep their authored blend |

`SceneOptions` holds only scalar/ref fields — **Papyrus structs can't have array members**, so named-role
binding stays in separate functions. `StartSceneRoles` is the compact compatibility form without
options; `StartSceneRolesEx` accepts the same binding plus `SceneOptions`.

Registered-scene launch compatibility differs between the two APIs:

| Start path | Out-of-range linear stage | Speed |
|---|---|---|
| Papyrus by ID or `StartSceneRolesEx` | Refuses the start | Seeds `SceneOptions.Speed` before playback |
| Native `StartScene` / `StartSceneRoles` | Falls back to the scene entry | Seeds `OSFStartOptions.speed` before playback |
| Papyrus matchmaking (`StartSceneByTags`, query, or anchor) | Always uses the authored entry | Seeds `SceneOptions.Speed` before playback |

For registered starts, stage `0` uses the authored entry; scenes without `linearStages` ignore the
stage option. Furniture-bound scenes require an accepted reference anchor even when the native
caller supplies explicit world coordinates.

**Per-start tri-state overrides (`StripMode` / `LockPlayerMode` / `PlayerControlMode` / `FadeMode` /
`InPlaceMode`).** Write these compatibility fields with
the `OSF.INHERIT()` (= -1, leave the scene/file default), `OSF.OFF()` (= 0, force off), and `OSF.ON()`
(= 1, force on) helpers — **not** bare `0`/`1`, because `0` means *force off*, not "leave default". An unset
field (default `-1`) inherits the scene's authored value. For `InPlaceMode`, "off" means canonical
anchor-and-pin placement rather than disabling placement. Disabling apparel hiding is undo-safe
(nothing recorded → nothing restored).

**`LoopScale`.** Multiplies the loop count of every *loop-driven* stage (`new = max(1, round(loops × scale))`),
so a terminal-driven `GlobalVariable` can lengthen/shorten scenes. It affects only stages that already loop a
fixed number of times; "loop until advance" (hold) and timer-only stages are untouched, so on a mixed graph
the felt effect is uneven. Sanitized: `≤ 0` / NaN → `1.0` (no scaling); clamped to a ceiling so a runaway
value can't mint a stage that never auto-advances. Re-applied on every flow-node entry (never compounded).
Scene JSON can also [read loop counts directly from a game global](SCENE_SCHEMA.md)
using `"loops": { "global": "Plugin.esm|0x1234", "default": 3 }` (1.5.0).
Those counts are captured at scene start/handoff, then multiplied by `LoopScale`; zero remains a hold.
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

Linear scenes (and graph scenes that declare `linearStages`) also support
`GetSceneStage`/`SetSceneStage` (by handle) and `GetSceneStageForActor`/`SetSceneStageForActor` (by
actor). These expose a **linear-stage index**, never an internal playback-segment index.

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
        ;                 actorRef, role, loopIndex, time, anchor, result, cause, sceneId
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
| `OSF.EVENT_SCENE_HANDOFF()` | 64 | the handle changed definition; `sceneId` is new, `edge` is the previous id |
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

Engine services **3.5** append `SetSceneAutoAdvance(handle, enabled)` and
`ReadSceneControls(handle, callback, context)`. Both require the game thread.
The automatic preference defaults to true and survives nodes and same-cast handoffs.
When false, nodes marked `allowManualHold` keep playing and delivering timed cues but do not
complete from loop or timer budgets. Other nodes still complete normally. Explicit navigation
and stop remain available. Re-enabling starts a fresh timer/loop budget at the current clip
phase, without replaying once-only cues. A completed held playback cannot be revived.

`ReadSceneControls` calls back synchronously with a JSON object containing `sceneId`, `node`,
`automatic`, `allowManualHold`, `speed`, and `edges` (available named advance edges as
`{ "id": "...", "label": "..." }`). It returns false for an invalid handle. Copy strings in
the callback. Consumers must recheck live state when applying a choice; snapshots are not commands.

## Owned audio channels

Engine services **3.6** append three any-thread operations:

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

Services **3.7** add `BindActorSoundChannel(actor, channel)` for consumers that must give authored
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

Services 3.8 adds `CreateAudioChannelWithOptions`, `AudioClockNs` and `PlayAudioChannelAt`.
Options are fixed for a channel's lifetime. `latestPending=1` retains at most one waiting job
per channel, replacing it in place before checking global queue capacity. Executing older jobs
recheck supersession before preparation and before submission. An admitted replacement that
later fails decoding does not revive discarded work; it also does not stop the playing voice.
Invalid specs and rejected admissions do not supersede pending work. Closing an opted channel
removes its waiting job immediately and fences executing work through the existing commit lock.

`maxAgeMs=0` disables expiry. A positive limit checks age before admission, before preparation,
and immediately before Wwise submission, even when there is no newer request. Use the scene 1.6
event's `detectedNs` for timed cues; it preserves the original monotonic mark-detection time
across the native frame queue. For other requests obtain a timestamp from `AudioClockNs` before
selection work. `PlayAudioChannelAt` rejects zero/future timestamps. Plain `PlayAudioChannel`
starts age at entry. These are submission limits, not audible-onset or sound-duration limits.
Detection time is not a reconstructed animation due time. The default `CreateAudioChannel`
continues without coalescing or expiry, including its existing failed-replacement behavior.

The scene ABI is 1.6. The separate `OSF_RequestEngineAPI` export provides services ABI 3.11
for verified-frame dispatch, lifecycle subscriptions, catalog/import metadata, matching, active state,
playback controls, owned audio channels, shared camera/input mechanisms, and actor-state scopes. See [BROWSER_ADDON.md](BROWSER_ADDON.md) and
`src/API/OSFEngineAPI.h` for ownership, callback lifetimes, cancellation, and JSON contracts. Gameplay
consumers need neither OSF Animation Browser nor OSF UI.

## Shared actor-state ownership

Services 3.9 adds persistent and session scopes with equipment, preservation, and required
or optional hiding claims. Scene role equipment, automatic gear, equipment actions and
stripping use the same service. Release is selective, save suspension retains persistent
demands, and world replacement revokes runtime-only tokens. See [ACTOR_STATE.md](ACTOR_STATE.md)
for the native API, conflicts, cleanup retry, and base-form inventory provenance limits.
Services 3.11 adds read-only `ReadActorEquipment` observations from the shared H2 backend; see [actor state](ACTOR_STATE.md).

Services 3.10 adds [shared preparation](PREPARATION.md), including versioned preparers,
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
loaded **scene** count for compatibility. `ReloadPacks` is the established Papyrus binding name; a
content pack is only an author/product grouping and is not the reload boundary.

## Primitives (advanced)

`Play` / `Stop` (solo clip), `SetSpeed` / `GetSpeed` (1.0 = authored, 0 = freeze),
`SetAnchor` / `ClearAnchor` (pin a solo playback session to a world point), `GetCurrentAnimation`, and linear
scene stage getters/setters by handle or actor.

## API stability policy

Pre-1.0 (`0.x`) the surface is still settling and may change between releases. From **1.0** on, natives
are never removed or re-signatured within a major version (minor versions only **add**). The
`OSFTypes:SceneEvent` struct member set is part of the ABI — new fields append at the end, so old
callbacks keep working. The native C++ API follows the same additive rule: minor versions append
vmethods or POD fields, while a major version is required for an incompatible layout change.
