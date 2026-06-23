# OSF Papyrus API

OSF exposes its surface as the global Papyrus script `OSF` (declared in
[dist/Scripts/Source/OSF.psc](../dist/Scripts/Source/OSF.psc)). Call it from any script —
`OSF.StartSceneByTags(...)`, `OSF.StopScene(...)`, etc. This doc is the integration guide; the
per-native reference is the doc-commented `OSF.psc` itself.

OSF is **content-neutral**: it provides playback/scene *mechanism* only. Gameplay policy — *when*,
*why*, and *with what odds* to start an interaction — belongs in your mod (the SIF framework is the
worked example; see [GETTING_STARTED.md](GETTING_STARTED.md)).

## Readiness

```papyrus
If OSF.IsReady()                  ; hooks installed + verified
    ...
EndIf
Bool ok = OSF.HasFeature("scenes")  ; "scenes"/"playback"/"sync"/"anchor"/"cues"/"actions"/
                                    ; "sound"/"camera"/"callbacks" — all report the engine-live state
String v = OSF.GetVersion()         ; semver "major.minor.patch"
```

## Handles

Every `Start*` returns an opaque **scene handle** (`Int`): `0` = failed (bad id, no match, or an
actor already in a live scene — one live scene per actor is enforced). A nonzero handle drives
navigation, callbacks, stop, and the state getters. When the scene ends, the handle goes invalid and
getters return sentinels (`""` / `0`).

## Starting scenes

```papyrus
Actor[] actors = new Actor[2]
actors[0] = akA
actors[1] = akB

; By id (scene def or pack; "scene:"/"anim:" prefixes force a registry):
int h = OSF.StartScene(actors, "author.scenes.demo")

; By matchmaking — tags + role/gender/keyword/race fit across scene defs AND packs,
; chosen by priority tier then weighted-random:
string[] tags = new string[1]
tags[0] = "takedown"
int h2 = OSF.StartSceneByTags(actors, tags)

; Boolean query form (allOf / anyOf / noneOf — pass real empty arrays, never None):
int h3 = OSF.StartSceneByTagsQuery(actors, allOf, anyOf, noneOf)

; World-anchored at a ref (furniture / bed / marker) instead of at actors[0]:
int h4 = OSF.StartSceneByTagsAt(actors, tags, akBed)               ; afHeadingDeg < 0 = ref's own heading
int h5 = OSF.StartSceneByTagsQueryAt(actors, allOf, anyOf, noneOf, akBed)
int h6 = OSF.StartSceneAt(actors, "author.scenes.demo", akBed)     ; by id, anchored
int h7 = OSF.StartSceneRoles(actors, "author.scenes.demo", roleNames)  ; bind actors to named roles
int h8 = OSF.StartSceneFiles(actors, files, 1.0, 0.4)              ; ad-hoc from raw clip paths
```

> **None-array footgun:** passing a `None` array into a native that expects an array can crash. Always
> pass a real (possibly empty) array — `new String[0]` is valid and accepted. Arrays you *receive* from
> OSF are always real.

## Stopping & state

```papyrus
OSF.StopScene(h)                 ; by handle (fires SCENE_END + runs the undo ledger)
OSF.StopSceneForActor(akActor)   ; by actor; false if it's in no scene
Bool playing = OSF.IsPlaying(akActor)
int  scene   = OSF.GetSceneForActor(akActor)   ; 0 if none
String id    = OSF.GetSceneId(h)
String node  = OSF.GetSceneNode(h)
```

Stopping always runs the **undo ledger**, which reverses every mechanism the scene engaged
(control/camera/weapon/equipment/fade) in reverse order, on *every* termination path. Consumers do not
clean up engaged mechanisms by hand.

## Navigation (def-backed scenes)

```papyrus
Bool moved = OSF.AdvanceScene(h)               ; take the current node's default advance edge
Bool went  = OSF.NavigateScene(h, "finish")    ; take a named branchable edge
int n = OSF.GetSceneEdgeCount(h)               ; branchable edges (for building a menu)
String eid = OSF.GetSceneEdgeId(h, 0)
String lbl = OSF.GetSceneEdgeLabel(h, 0)
```

Linear scenes (packs / `linearStages`) also support `GetSceneStage`/`SetSceneStage` (by handle) and
`GetSceneStageForActor`/`SetSceneStageForActor` (by actor).

## Scene-event callbacks

Register a receiver to get `OSFEvent:SceneEvent` structs (see
[dist/Scripts/Source/OSFEvent.psc](../dist/Scripts/Source/OSFEvent.psc)). Dispatch is **asynchronous**
— the payload is a snapshot struct (no dispatch-time getters).

```papyrus
; aiScene 0 = any scene; aiEventMask is a bitmask of OSF.EVENT_*().
int token = OSF.RegisterSceneCallback(Self, "OnSceneEvent", 0, OSF.EVENT_ALL())
...
OSF.UnregisterSceneCallback(token)

Function OnSceneEvent(OSFEvent:SceneEvent akEvent)
    If akEvent.eventType == OSF.EVENT_SCENE_END()
        Actor a = akEvent.actorRef
        ; akEvent fields: sceneHandle, eventType, node, edge, cue, actionType,
        ;                 actorRef, role, loopIndex, time, anchor, result
    EndIf
EndFunction
```

**Event-type bits** (compose into the mask; exposed as functions so they read on the type, `OSF.X()`):

| Function | Bit | Fires when |
|----------|----:|------------|
| `OSF.EVENT_NODE_ENTER()` | 1 | a node is entered (also dispatches that node's enter cues) |
| `OSF.EVENT_NODE_EXIT()` | 2 | a node is exited |
| `OSF.EVENT_CUE()` | 4 | a `cue` track entry fires |
| `OSF.EVENT_ACTION()` | 8 | a custom (non-`osf.*`) action fires |
| `OSF.EVENT_ACTION_FAILED()` | 16 | a custom action could not be delivered |
| `OSF.EVENT_SCENE_END()` | 32 | the scene ended normally |
| `OSF.EVENT_SCENE_ABORT()` | 64 | the scene was aborted |
| `OSF.EVENT_ALL()` | 65535 | every type |

`OSF.RESULT_OK()` / `RESULT_BAD_ROLE()` / `RESULT_RUNTIME_FAILURE()` / `RESULT_NO_HANDLER()` decode
`akEvent.result`.

## Discovery & diagnostics

```papyrus
string[] ids   = OSF.FindScenes(2, tags)                              ; filter-UNAWARE hint
string[] ids2  = OSF.FindScenesForActorsQuery(actors, allOf, anyOf, noneOf)  ; filter-aware
string[] roles = OSF.GetSceneRoles("author.scenes.demo")
string gender  = OSF.GetSceneRoleGender("author.scenes.demo", "lead")
string[] sTags = OSF.GetSceneTags("author.scenes.demo")
int n          = OSF.GetSceneActorCount("author.scenes.demo")
Bool valid     = OSF.ValidateScene("author.scenes.demo")
string[] errs  = OSF.GetSceneLoadErrors()                            ; [error]/[warn] prefixed
string[] e2    = OSF.GetSceneValidationErrors("author.scenes.demo")
int count      = OSF.ReloadPacks()                                   ; rescan Data/OSF, return count
```

## Primitives (advanced)

`Play` / `Stop` (solo clip), `SetSpeed` / `GetSpeed` (1.0 = authored, 0 = freeze),
`SetAnchor` / `ClearAnchor` (pin a solo graph to a world point), `Sync` (frame-lock already-playing
graphs), `PlaySequence` (solo multi-phase), `GetCurrentAnimation`.

## API stability policy

Pre-1.0 (`0.x`) the surface is still settling and may change between releases. From **1.0** on, natives
are never removed or re-signatured within a major version (minor versions only **add**). The
`OSFEvent:SceneEvent` struct member set is part of the ABI — new fields append at the end, so old
callbacks keep working.
