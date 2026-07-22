> WARNING: This file was AI-generated and is likely incorrect. Treat it as a rough draft, not authoritative documentation.

# OSF Papyrus API

Native SFSE plugins that need per-save records should use the [shared persistence C ABI](RFC-persistence-api.md).

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
String v = OSF.GetVersion()         ; semver "major.minor.patch"
```

## Handles

Every `Start*` returns an opaque **scene handle** (`Int`): `0` = failed (bad id, no match, or an
actor already in a live scene — one live scene per actor is enforced). A nonzero handle drives
navigation, callbacks, stop, participant lookup, and linear-stage getters. When the scene ends, the
handle goes invalid except for the short roster-survival window described under callbacks.

## Starting scenes

There are **three public ways to start a registry scene**. Each takes an optional trailing
`SceneOptions` struct (`None` = all defaults) for anchoring and per-start policy overrides, so the
common case stays a one-liner.

```papyrus
Actor[] actors = new Actor[2]
actors[0] = akA
actors[1] = akB

; By id (a single registry lookup — one scene namespace, no prefixes, no fallback):
int h = OSF.StartScene(actors, "author.scenes.demo")

; By matchmaking — tags + role/gender/keyword/race fit across the one scene registry,
; chosen by priority tier then weighted-random:
string[] tags = new string[1]
tags[0] = "takedown"
int h2 = OSF.StartSceneByTags(actors, tags)

; Boolean query form (allOf / anyOf / noneOf — pass real empty arrays, never None):
int h3 = OSF.StartSceneByTagsQuery(actors, allOf, anyOf, noneOf)
```

For raw one-actor clip playback outside the scene registry, use the primitive `OSF.Play(actor, file)`.

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

Dynamic file specs accept the same compatibility shortcuts as scene JSON: `naf:Path.glb` resolves under `Data/NAF`, and `File.glb:AnimName` selects a named GLB animation.

`StartSceneFilesPlaced` / `StartSceneStagesPlaced` add dynamic participant placement offsets without JSON. `x`/`y`/`z` are local offsets relative to the scene anchor, and `headingDeg` is relative facing in degrees. Each placement array may be empty, which means zero for that component. For `StartSceneFilesPlaced`, non-empty placement arrays must be actor-count length. For `StartSceneStagesPlaced`, non-empty placement arrays may be actor-count length (reused for every stage) or stage-major length matching `stageMajorFiles`.

`HideEquipment(scene, actor, slotMask)` is a ledger-safe ad-hoc strip helper for dynamic starts. The actor must be a participant in `scene`; `slotMask` uses Starfield ARMO biped slot bits, `-1` hides all apparel, and `0` hides nothing. Hidden items are restored automatically when the scene ends, or early with `RestoreEquipment(scene)`.

`SceneOptions` carries the optional modifiers (set only the fields you need; each `Start*` reads only
the ones that apply to it):

| Field | Type | Applies to | Meaning |
|---|---|---|---|
| `Anchor` | `ObjectReference` | StartScene, StartSceneByTags(Query) | world-anchor at a ref (furniture/bed/marker) instead of co-locating at `actors[0]` |
| `HeadingDeg` | `Float` | (with `Anchor`) | anchor facing in degrees; `< 0` = the ref's own heading |
| `StripMode` | `Int` | StartScene, StartSceneByTags(Query) | override the scene's strip-actors policy: `OSF.INHERIT()`/`OFF()`/`ON()` |
| `LockPlayerMode` | `Int` | StartScene, StartSceneByTags(Query) | override the player-input lock: `OSF.INHERIT()`/`OFF()`/`ON()` |
| `FadeMode` | `Int` | StartScene, StartSceneByTags(Query) | override the optional start fade-to-black curtain: `OSF.INHERIT()`/`OFF()`/`ON()` |
| `LoopScale` | `Float` | StartScene, StartSceneByTags(Query) | multiply every loop-driven stage's loop count (`1.0` = unchanged); see below |
| `Stage` | `Int` | — | **not wired** for graph scenes in this build; use `SetSceneStageForActor()` after start |
| `Speed` / `BlendIn` | `Float` | — | reserved for an internal files-start path; currently **no-ops** for public callers |

`SceneOptions` holds only scalar/ref fields — **Papyrus structs can't have array members**, so named-role
binding stays its own function, `StartSceneRoles` (which therefore takes no `SceneOptions` — overrides
don't apply to the roles path yet).

**Per-start overrides (`StripMode` / `LockPlayerMode` / `FadeMode`).** These are tri-state: write them with
the `OSF.INHERIT()` (= -1, leave the scene/file default), `OSF.OFF()` (= 0, force off), and `OSF.ON()`
(= 1, force on) helpers — **not** bare `0`/`1`, because `0` means *force off*, not "leave default". An unset
field (default `-1`) inherits the scene's authored value. Disabling strip is undo-safe (nothing recorded →
nothing restored).

**`LoopScale`.** Multiplies the loop count of every *loop-driven* stage (`new = max(1, round(loops × scale))`),
so a terminal-driven `GlobalVariable` can lengthen/shorten scenes. It affects only stages that already loop a
fixed number of times; "loop until advance" (hold) and timer-only stages are untouched, so on a mixed graph
the felt effect is uneven. Sanitized: `≤ 0` / NaN → `1.0` (no scaling); clamped to a ceiling so a runaway
value can't mint a stage that never auto-advances. Re-applied on every node entry (never compounded).

> **Capability note:** the DLL and a consumer's compiled scripts ship independently. A consumer compiled
> against a newer `OSFTypes` but running an older DLL will have these fields silently ignored (the old native
> doesn't read them). Gate on `OSF.GetVersion()` if you need to know the override fields are honored.

```papyrus
; World-anchored at a ref (the old StartSceneByTagsAt / StartSceneAt):
OSFTypes:SceneOptions opts = new OSFTypes:SceneOptions
opts.Anchor = akBed                                           ; opts.HeadingDeg stays -1.0 = bed's heading
int h5 = OSF.StartScene(actors, "author.scenes.demo", opts)   ; by id, anchored
int h6 = OSF.StartSceneByTags(actors, tags, opts)             ; matchmade, anchored

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

Linear scenes (those with `linearStages`) also support `GetSceneStage`/`SetSceneStage` (by handle) and
`GetSceneStageForActor`/`SetSceneStageForActor` (by actor).

## Scene-event callbacks

Register a receiver to get `OSFTypes:SceneEvent` structs (see
[dist/Scripts/Source/OSFTypes.psc](../dist/Scripts/Source/OSFTypes.psc)). Dispatch is **asynchronous**
— the payload is a snapshot struct (no dispatch-time getters).

> **Participants at scene end:** the one exception to "no live getters" — the event's
> `sceneHandle` stays *roster-queryable* through the `EVENT_SCENE_END` callback, so
> `OSF.GetSceneParticipants(akEvent.sceneHandle)` returns who took part even though the scene has
> ended (the retired roster is retained for the loaded world). `SCENE_END` carries no
> `actorRef` itself, so this is how an end handler enumerates participants. Note `SCENE_END` fires on
> runtime termination (normal finish or `Stop()`). World-replacing load teardown clears the VM relay
> and native handle table without dispatching callbacks into the discarded world. Gate on a completion cue if you
> only want genuine finishes.

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
| `OSF.EVENT_SCENE_END()` | 16 | the scene ended (normal end or `Stop()`) |
| `OSF.EVENT_SCENE_BEGIN()` | 32 | the scene started (fires once, before the entry node's `NODE_ENTER`) |
| `OSF.EVENT_ALL()` | 65535 | every type |

`SCENE_BEGIN` is the lifecycle-open bookend of `SCENE_END`: it fires exactly once per scene as the
first event, after OSF has applied start setup (player lock, strip, role equip, optional fade,
input channel) and the entry node's animation is playing — so the scene is fully live. Its `node`
field carries the entry node; like `SCENE_END` it carries no `actorRef`. The handle is live when
`SCENE_BEGIN` is dispatched; because dispatch is async (the callback runs on a later VM tick), it
carries the same roster-survival guarantee as `SCENE_END` — `GetSceneParticipants(akEvent.sceneHandle)`
stays readable for the callback, but a getter may return empty for a very short (`once`/0-duration)
scene that already ended before the queued callback ran.

`OSF.RESULT_OK()` / `RESULT_BAD_ROLE()` / `RESULT_RUNTIME_FAILURE()` / `RESULT_NO_HANDLER()` decode
`akEvent.result`.

## In-game UI, hotkeys, and the wheel tag contract

The scene browser, animation wheel, settings menu, and hotkeys are hosted by **OSF UI** (a separate
mod) — there is no Papyrus surface for them, and no `Data/OSF/settings.json` (it is no longer read).
Hotkeys are bound in OSF UI's settings menu on the **OSF Animation** card: the wheel defaults to `B`;
the browser key ships unbound because OSF UI's own console toggle (F10) already opens it.

Content packs join these surfaces purely by **tags**, no code: a solo, free-space, self-terminating
scene tagged `player.emote.<name>` appears under Animations → Emotes and in the default animation
wheel. The well-known tag contract lives in [SCENE_SCHEMA.md](SCENE_SCHEMA.md).

## Discovery & diagnostics

```papyrus
int count = OSF.ReloadPacks()  ; rescan Data/OSF/**/*.osf.json and *.sounds.json
```

`ReloadPacks()` rebuilds the **one** scene registry, reloads sound pools, clears clip import caches, and
returns the loaded scene count. The native keeps its `ReloadPacks` name for the existing Papyrus
binding; there are no separate "packs" anymore.

## Primitives (advanced)

`Play` / `Stop` (solo clip), `SetSpeed` / `GetSpeed` (1.0 = authored, 0 = freeze),
`SetAnchor` / `ClearAnchor` (pin a solo graph to a world point), `GetCurrentAnimation`, and linear
scene stage getters/setters by handle or actor.

## API stability policy

Pre-1.0 (`0.x`) the surface is still settling and may change between releases. From **1.0** on, natives
are never removed or re-signatured within a major version (minor versions only **add**). The
`OSFTypes:SceneEvent` struct member set is part of the ABI — new fields append at the end, so old
callbacks keep working.
