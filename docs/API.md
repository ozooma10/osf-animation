# OSF Papyrus API — consumer guide & stability policy

OSF Animation is the **content-neutral animation playback core**. Its API moves bones,
synchronizes actors, anchors them in the world, and runs mechanical staged scenes —
nothing about *what* an animation is for. Scene **policy** (undress, scheduled voice,
camera/control, fade choreography, scene/cue callbacks) is **not here** — it lives in
the separate **OSF Intimacy** scene engine that builds on this core.

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

; --- mechanical scene by registry id (anchored, staged, synced) ---------------
Actor[] a = new Actor[2]
a[0] = first      ; fills the definition's actor slot 0
a[1] = second
OSF.StartScene(a, "author.pack.bridge")        ; 1..N actors; a 1-actor def is a real scene

; matchmake by tags + gender slots (returns the chosen id, or ""):
string played = OSF.StartSceneByTags(a, tags)

; ad-hoc scene straight from files (co-locates a[1..] at a[0], anchors, syncs):
string[] files = new string[2]
files[0] = "OSF\\Anims\\bridgeA.glb"
files[1] = "OSF\\Anims\\bridgeB.glb"
OSF.StartSceneFiles(a, files)

OSF.StopScene(a[0])   ; any participant identifies the scene

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
per-participant offsets, frame-locks their clocks, runs the pack's stages (timer / loop
auto-advance), and pins the rendered skeletons each frame. That is the *whole* contract —
no undress, no voice, no camera, no fade. Rule of thumb: one actor, bones only, world
unchanged → a primitive; anything that coordinates actors or anchors them → a scene.
(Anchoring/rootMode: [docs/ANCHORING.md](ANCHORING.md). Design rationale: DESIGN.md §7–8.)

**Where did policy go?** Undress/redress, scheduled voice, camera/control, fade
choreography, and scene/cue callbacks are **OSF Intimacy** — a separate scene engine that
depends on this core. If your mod needs intimate-scene orchestration, build on OSF
Intimacy; if you need reliable native playback (machinima, dance, NPC vignettes, custom
scene logic), build on this core directly.

## State getters

`GetSceneStage(actor)` (-1 = not in a scene) · `IsPlaying(actor)` (includes fade-outs) ·
`GetCurrentAnimation(actor)` (source path, or "") · `GetSpeed(actor)` · `GetVersion()`.

## Readiness handshake

Gate on OSF as an optional dependency:

- `OSF.IsReady()` — loaded + playback hooks installed.
- `OSF.HasFeature(name)` — a **single aggregate gate**: `"scenes"`/`"playback"`/`"sync"`/
  `"anchor"` all report the same state (the two playback hooks installed + verified on this
  game build; they self-disable together on a version mismatch rather than crash). Unknown
  name → false. Treat it as one "is OSF's engine layer live?" check, not per-feature probing.
- `OSF.GetVersion()` — semver.

## Save-safety contract

On a world-replacing load the engine resets every actor to the saved state, so the core
**drops all in-memory scene/graph state** (it is anchored in the world that was just
discarded) — via a SaveLoadEvent sink, a TESLoadGameEvent backstop, and the manual
`OSF.NotifyGameLoaded()` fallback (read its warning in OSF.psc first). The backstop also
re-binds the natives onto the rebuilt Papyrus VM. The core does **not** persist any
scene aftermath across saves and never resumes playback from a save — replay from your
own quest state if needed. (Cross-restart redress/resume is an OSF Intimacy concern.)

## Compatibility natives (`OSFCompat`)

`OSFCompat.SetPlayerControlLock(bool)` and `SetPlayerCameraLock(bool)` are
standalone player locks (input-disable layer + AI-driven; force/hold third person) for
the SAF shim's primitive (non-Scene) Play+Sync path. They are a content-neutral
*mechanism* the core never auto-applies — call them explicitly if a player-participant
scene needs the body frozen. New integrations generally don't need them.

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
(`RegisterForPhaseBegin`/`RegisterForSequenceEnd` never fire), the crosshair pickers + selection
buffer, blend-graph variables (`Set`/`GetBlendGraphVariable`), and absolute positioning
(`SetActorPosition`/`MatchActorTransform`). SAF content that drives gameplay off those will not
behave identically; playback-driven SAF content does.
