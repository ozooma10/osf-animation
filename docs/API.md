# OSF APIs

Native SFSE plugins that need per-save records should use the [shared persistence C ABI](PERSISTENCE_API.md).

Domain terms in this guide follow [OSF domain vocabulary](VOCABULARY.md). The canonical JSON aliases
documented in [SCENE_SCHEMA.md](SCENE_SCHEMA.md) do not rename existing Papyrus or native API fields;
names shown in code remain compatibility spellings.

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
1.3 (the header default); an older OSF build returns `nullptr` instead of exposing a short vtable.

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
```

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
unified load-problem list and can include route and curated-animation problems as well as scene
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
terminates, or early with `RestoreEquipment(scene)`.

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
| `Speed` / `BlendIn` | `Float` | StartSceneFiles(Placed), StartSceneStages(Placed) | set the ad-hoc playback plan's speed and default blend-in; registered scene definitions keep their authored values |

`SceneOptions` holds only scalar/ref fields — **Papyrus structs can't have array members**, so named-role
binding stays in separate functions. `StartSceneRoles` is the compact compatibility form without
options; `StartSceneRolesEx` accepts the same binding plus `SceneOptions`.

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

> **Participants at scene end:** the one exception to "no live getters" — the event's
> `sceneHandle` stays *roster-queryable* through the `EVENT_SCENE_END` callback, so
> `OSF.GetSceneParticipants(akEvent.sceneHandle)` returns who took part even though the scene has
> ended. Retired rosters use a bounded recent-history window (currently 256), so copy the roster
> during the callback rather than caching the handle indefinitely. `SCENE_END` carries no
> `actorRef` itself, so this is how an end handler enumerates participants. Note `SCENE_END` fires on
> runtime termination paths that dispatch callbacks, including natural completion and `Stop()`
> cancellation. The current event ABI does not carry a complete/cancel/interrupt cause.
> World-replacing load teardown clears the VM relay and native handle table without dispatching
> callbacks into the discarded world. Native C++
> registrations remain registered, but receive no teardown event. Gate on a completion cue if you
> only want proof of natural completion.

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
        ;                 actorRef, role, loopIndex, time, anchor, result
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
| `OSF.EVENT_SCENE_END()` | 16 | the scene terminated on a callback-emitting path; cause is not encoded |
| `OSF.EVENT_SCENE_BEGIN()` | 32 | the scene started (fires once, before the entry flow node's `NODE_ENTER`) |
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

## In-game UI and hotkeys

The scene browser, settings menu, and hotkeys are hosted by **OSF UI** (a separate
mod) — there is no Papyrus surface for them, and no `Data/OSF/settings.json` (it is no longer read).
The browser hotkey is bound in OSF UI's settings menu on the **OSF Animation** card. It ships unbound so it
do not replace Starfield's context-sensitive or localized key assignments; OSF UI's own console
toggle (F10) already opens the browser.

Content packs join the browser purely by **tags**, no code: a scene tagged `player.emote.<name>`
appears as an **emote** in the unified Browse catalog. Older browser copy may label this playable category `Action`; that legacy
UI term is unrelated to a scene's authored `action` track. The well-known tag contract lives in
[SCENE_SCHEMA.md](SCENE_SCHEMA.md).

## Discovery & diagnostics

```papyrus
int count = OSF.ReloadPacks()  ; rescan Data/OSF/**/*.osf.json and *.sounds.json
```

`ReloadPacks()` **reloads content**: it rebuilds the content registry (scenes, routes, curated animation
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

## Native C++ overlay API

`src/API/OSFOverlayAPI.h` is the copyable native ABI for Phase-1 actor overlays. Request ABI 1.0
through `OSF_RequestOverlayAPI`, acquire one owner record for the consumer plugin id, then call
`BeginRoute` and `RequestStation`. `BeginRoute` returns `OSFOverlayBeginResult`: a nonzero
`routeHandle`, an `accepted`/`pending`/`rejected` disposition, and an exact reason. Rejections
distinguish `ownerInvalid`, `routeUnknown`, `unknownStation`, `busy`, `actorUnavailable`, and
`playbackFailed`. A scene- or 3D-blocked route returns a live handle with `pending` and the
corresponding blocker reason. Route handles are capabilities retained by their consumer; OSF does
not expose another owner's actor-to-handle lookup.

The owner callback acknowledges `kCommit` handoffs and receives informational `kMarker`,
`kPropAttach`, `kPropDestroy`, `kReached`, `kFailed`, `kSuspended`, `kResumed`, and `kEnded` events.
Marker ids use `event.marker`; typed external-prop events use `event.prop`, so consumers never infer
an operation from a marker name. Markers are instantaneous and sounds are one-shot; only props have
route lifetimes. `ReleaseOwner` is a quiescent callback barrier; route handles are generational and
world-local, while owner registration survives world replacement. See
[`ROUTE_SCHEMA.md`](ROUTE_SCHEMA.md) for authoring, interruption, lifetime, and one-live-route-per-actor
rules. The schema value `interrupt: "finish"` is a legacy spelling meaning complete the current route
transition before honoring the new station request; it does not end the route.

API calls are game-thread calls and are rejected while an owner callback is running; schedule route
mutations after the callback returns. Event strings are borrowed for the callback only, and query
strings remain valid only until the next overlay mutation. When a scene claims the actor, Phase 1
stops the overlay immediately before scene admission: the current one-stamper architecture cannot
crossfade two independently owned playback sessions.
