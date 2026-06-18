# OSF Papyrus API — consumer guide & stability policy

> **Direction note.** The scene-engine **merge has landed**: the scene runtime (a node graph with a
> loop-relative track timeline — navigation, cues, actions, callbacks) is now an internal subsystem
> in this mod, **additive** on the frozen Tier-0 primitives documented below. The Tier-0 natives
> remain canonical and stable; the scene API (Tier 1 *run a scene*, Tier 2 *author a scene*) is
> specified in [SCENE_DESIGN.md](SCENE_DESIGN.md) and is **beta** pre-1.0.

OSF Animation is the OSF **engine**. Tier 0 moves bones, synchronizes actors, anchors them in the
world, and runs mechanical staged scenes; the scene runtime (Tier 1+) adds graphs of nodes with
cues, actions, and callbacks. It stays **content-neutral**: it provides the policy *mechanisms*
(player control/camera lock, fade, equipment, scheduled voice — named neutrally), while specific
adult content and orchestration live in the separate **OSF Seduce** content mod.

The **canonical per-native reference is `Scripts/Source/OSF.psc`** — every native is
documented at its declaration (units, defaults, gotchas, return semantics). This page is
the integration guide and the compatibility contract.

## Stability policy

- **Semver.** `OSF.GetVersion()` returns `"major.minor.patch"`.
- **Within a major version: natives are never removed or re-signatured.** New capability
  arrives as new natives or new getters.
- **Pack schema** is versioned independently (`schema` field; see
  [PACK_SCHEMA.md](PACK_SCHEMA.md)) and only bumps on breaking changes.
- Anything not exposed through `OSF.psc` or the pack schema is internal and may change.

## Quickstart: play something

```papyrus
If !OSF.IsReady()
    Return   ; framework absent / hooks not installed — degrade gracefully
EndIf

; --- scene by registry id (anchored, staged, synced) -------------------------
; Start* return an opaque int HANDLE (0 = failed); pass it to StopScene / the getters / navigation.
Actor[] a = new Actor[2]
a[0] = first      ; fills the definition's actor slot 0
a[1] = second
int h = OSF.StartScene(a, "author.pack.bridge")   ; 1..N actors; a 1-actor def is a real scene

; matchmake by tags + role/gender(+keyword/race) fit across scene defs AND packs (priority + weighted
; pick; returns a handle, 0 = no match; GetSceneId(h) recovers the id):
int h2 = OSF.StartSceneByTags(a, tags)
; boolean-tag form: allOf / anyOf / noneOf
int h2b = OSF.StartSceneByTagsQuery(a, allOfTags, anyOfTags, noneOfTags)

; world-anchor a scene at a thing (bed/chair/marker) instead of at a[0] — furniture/sleep encounters:
int hb = OSF.StartSceneAt(a, "author.scenes.bedrest", akBedRef)   ; afHeadingDeg defaults to the ref's heading

; ad-hoc scene straight from files (co-locates a[1..] at a[0], anchors, syncs):
string[] files = new string[2]
files[0] = "OSF\\Anims\\bridgeA.glb"
files[1] = "OSF\\Anims\\bridgeB.glb"
int h3 = OSF.StartSceneFiles(a, files)

OSF.StopScene(h)                ; stop by handle
; OSF.StopSceneForActor(a[0])   ; ...or by any participant

; --- bare animation primitive (bones only, no scene side-effects) -------------
OSF.Play(npc, "OSF\\MyDance.glb")              ; plays in place, follows the actor
OSF.Stop(npc)
```

**Two layers, both content-neutral.** *Primitives* (`Play`, `Stop`, `Sync`,
`SetAnchor`/`ClearAnchor`, `SetSpeed`/`GetSpeed`, `GetCurrentAnimation`, `IsPlaying`,
`PlaySequence`) move bones and nothing else — the actor is not teleported and the world
is untouched; you position actors yourself, then `Sync` puts already-playing graphs on
one shared clock. *Scenes* (`StartScene` / `StartSceneByTags` / `StartSceneFiles`) are
**mechanical productions**: the core co-locates the actors at the anchor with the pack's
per-participant offsets, frame-locks their clocks, runs the stages (timer / loop auto-advance),
and pins the rendered skeletons each frame. A bare pack scene carries **no policy** — undress,
voice, camera, fade are opt-in `action`/`sound`/`camera` track entries an authored `*.scene.json`
adds (see "Where's the policy?" below). Rule of thumb: one actor, bones only, world unchanged →
a primitive; anything that coordinates actors or anchors them → a scene.
(Anchoring/rootMode: [docs/ANCHORING.md](ANCHORING.md). Design rationale: [SCENE_DESIGN.md](SCENE_DESIGN.md).)

**Where's the policy?** The content-neutral policy *mechanisms* — control/camera lock, fade,
equipment-strip, scheduled voice — are built-in `action`-track entries in the scene runtime, here
(see [SCENE_DESIGN.md](SCENE_DESIGN.md) §1.3). Scene/cue callbacks are the `RegisterSceneCallback`
event system, also here. What lives *elsewhere* is specific adult content + orchestration — the
**OSF Seduce** content mod. For reliable native playback (machinima, dance, NPC vignettes, custom
scene logic), the Tier-0 primitives and scenes are all you need.

## State getters

`GetSceneStage(handle)` / `GetSceneStageForActor(actor)` (-1 = non-linear, invalid, or not in a
scene) · `IsPlaying(actor)` (includes fade-outs) · `GetCurrentAnimation(actor)` (source path, or "")
· `GetSpeed(actor)` · `GetVersion()`.

**Scene-metadata introspection (read-only, by scene id).** Inspect a `*.scene.json` scene's
conventions before binding actors: `GetSceneRoles(id)` (role names) · `GetSceneRoleGender(id, role)`
(`"male"`/`"female"`/`"any"`) · `GetSceneActorCount(id)` · `GetSceneTags(id)`. An unknown id (or a
pack id — packs aren't scene defs) returns the empty/sentinel result; the arrays are safe to receive.

## Readiness handshake

Gate on OSF as an optional dependency:

- `OSF.IsReady()` — loaded + playback hooks installed.
- `OSF.HasFeature(name)` — a **single aggregate gate**: `"scenes"`/`"playback"`/`"sync"`/
  `"anchor"` (and the scene-runtime capabilities `"cues"`/`"actions"`/`"sound"`/`"camera"`/
  `"callbacks"`/`"weapon"`) all report the same state (the two playback hooks installed + verified
  on this game build; they self-disable together on a version mismatch rather than crash). Unknown
  name → false. Treat it as one "is OSF's engine layer live?" check, not per-feature probing.
- `OSF.GetVersion()` — semver.

## Compatibility natives (`OSFCompat`)

`OSFCompat.SetPlayerControlLock(bool)` and `SetPlayerCameraLock(bool)` are
standalone player locks (input-disable layer + AI-driven; force/hold third person) for
the SAF shim's primitive (non-Scene) Play+Sync path. They are a content-neutral
*mechanism* the core never auto-applies — call them explicitly if a player-participant
scene needs the body frozen. New integrations generally don't need them.

## Matchmaking (tags + role filters)

`StartSceneByTags` / `StartSceneByTagsQuery` matchmake across **both** composed `*.scene.json` scene
defs and animation packs. A scene def carries `priority` (tier) + `weight` (weighted-random within the
top tier) and per-role `filters` (`gender`, plus `keyword`/`race` as `"Plugin.esm|0xLocalID"` form
refs); a pack is a `priority 0` / `weight 1` pseudo-candidate, shadowed by a same-id scene def. Role
binding is deterministic complete matching, and filters are enforced on every scene-def start path
(`StartScene`/`StartSceneAt`/`StartSceneRoles` too). **Behavior change:** because matchmaking now spans
scene defs, an existing pack-only caller may receive scene-def ids and priority-based (not uniform)
selection once any matching scene def is installed — set `priority > 0` on a scene to intentionally
supersede packs. `FindScenes` (count+tags) is a **filter-unaware discovery hint**; for a filter-correct
list use `FindScenesForActorsQuery` (takes the actors) or let `StartSceneByTags*` bind.

## Gotchas that will actually bite you

- **Never pass `None` for an array parameter** — it asserts in the native binding layer.
  Pass real (possibly empty) arrays.
- `StopScene` for scene participants; `Stop` refuses them by design.
- Re-gate on `OSF.IsReady()` after a load if you cache framework presence.
- Anchored scenes set participants animation-driven; the core reverts that on
  `StopScene`. A player participant's rendered heading is pinned per-frame.

## Worked examples

The `OSFTest.psc` script shipped in `Scripts/Source` is a complete minimal consumer:
console-callable wrappers for every primitive and scene entry. `SAF.psc` / `SAFScript.psc`
are the SAF compatibility shim — existing SAF **playback/sync/scene** content runs unchanged
by forwarding `Ping→IsReady`, `PlaySceneSeparate→StartSceneFiles`,
`StopAnimation→Stop`/`StopScene`, `SyncGraphs→Sync`.

**SHIM-GAPs (what does *not* carry over).** SAF entry points the content-neutral core has no
equivalent for are inert stubs that log and no-op: phase/sequence-end callbacks
(`RegisterForPhaseBegin`/`RegisterForSequenceEnd` never fire), the multi-actor selection buffer
(`AddActorToSelectionBuffer`/`GetSelectionBuffer`), blend-graph variables
(`Set`/`GetBlendGraphVariable`), and absolute positioning
(`SetActorPosition`/`MatchActorTransform`). SAF content that drives gameplay off those will not
behave identically; playback-driven SAF content does. (The single crosshair target —
`GetCrosshairRef`/`GetCrosshairActor` and the crosshair pickers — is now native, via
`OSFCompat` reading `PlayerCharacter->commandTarget`.)
