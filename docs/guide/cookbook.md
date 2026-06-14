# OSF Cookbook

Task-oriented recipes. Every function is a `Global` native on the `OSF` script — call
`OSF.Foo(...)`. Gate everything on `OSF.IsReady()` (see the [guide overview](index.md)).
Full per-argument docs live at each native's declaration in `Scripts/Source/OSF.psc`.

This is the **content-neutral core**: it plays, syncs, anchors, and stages. Undress, voice,
camera, fades, and scene callbacks are the **OSF Intimacy** scene engine, not here (see the
end of this page).

---

## Play animations (primitives)

**Play one animation on an actor — in place (dance, idle, pose):**
```papyrus
OSF.Play(akActor, "OSF\\MyMod\\dance.glb")   ; loops; the actor keeps moving/colliding normally
OSF.Stop(akActor)                            ; fade out (0.4s default)
```
Optional args: `OSF.Play(akActor, file, asClip)` — a clip name/index inside a multi-clip file.

**Slow down, speed up, or freeze:**
```papyrus
OSF.SetSpeed(akActor, 0.5)   ; half speed
OSF.SetSpeed(akActor, 0.0)   ; freeze on the current frame
OSF.SetSpeed(akActor, 1.0)   ; authored speed
```

**Frame-lock several already-playing animations (a synced troupe in formation):**
```papyrus
OSF.Play(a, "...")
OSF.Play(b, "...")
Actor[] group = new Actor[2]
group[0] = a
group[1] = b
OSF.Sync(group)              ; they share one clock from here (<= 4 actors)
```
No teleport — you place the actors yourself first; `Sync` only locks their timing.

**Pin an animation to a fixed spot (lock the rendered pose to a world point):**
```papyrus
OSF.SetAnchor(akActor, x, y, z, headingDeg, 0)   ; rootMode 0 = pin (lock here)
OSF.ClearAnchor(akActor)                          ; back to following the actor
```
`rootMode`: `0` pin · `1` additive (root motion travels from the point) · `2` follow.
See [ANCHORING.md](../ANCHORING.md). Refused for scene participants (their placement is
scene-driven).

**Play an ordered multi-phase sequence on one actor (intro → loop → outro):**
```papyrus
string[] files = new string[3]
files[0] = "OSF\\MyMod\\intro.glb"
files[1] = "OSF\\MyMod\\loop.glb"
files[2] = "OSF\\MyMod\\outro.glb"
int[] loops = new int[3]       ; times each phase loops before advancing
loops[0] = 1
loops[1] = 4
loops[2] = 1
float[] blends = new float[3]  ; blend-in seconds per phase
blends[0] = 0.3
blends[1] = 0.3
blends[2] = 0.3
OSF.PlaySequence(akActor, files, loops, blends, false)  ; last arg true = loop the whole sequence
```
Primitive: in place, follows the actor. Arrays must be equal length.

**See what's playing:**
```papyrus
string file = OSF.GetCurrentAnimation(akActor)   ; "" if nothing
bool playing = OSF.IsPlaying(akActor)            ; includes fade-outs
```

---

## Run scenes (mechanical productions)

**Start a scene from an installed pack — the common case:**
```papyrus
Actor[] a = new Actor[2]
a[0] = first    ; fills the pack definition's actor slot 0
a[1] = second
OSF.StartScene(a, "author.pack.bridge")
```
Works for 1..N actors (a 1-actor def is a full 1-participant scene). The pack supplies
clips, stages, and alignment offsets; OSF anchors, stages, and syncs. Stop with
`OSF.StopScene(a[0])`.

**Let OSF pick a scene by tags instead of a fixed id:**
```papyrus
string[] tags = new string[2]
tags[0] = "paired"
tags[1] = "standing"
string id = OSF.StartSceneByTags(a, tags)   ; returns the chosen id, or "" if none matched
```
Matchmakes on actor count + tags + each actor's gender slot — you bind to a *concept*,
not one pack's id.

**Start a scene from raw files (no pack needed):**
```papyrus
string[] files = new string[2]
files[0] = "OSF\\MyMod\\a.glb"
files[1] = "OSF\\MyMod\\b.glb"
OSF.StartSceneFiles(a, files, 1.0, 0.4)   ; speed, blend-in
```
OSF co-locates the actors, anchors, syncs, and plays in one call. For explicit
per-actor placement, use the primitive path instead (`Play` + `SetAnchor` + `Sync`).

**Jump stages / stop:**
```papyrus
OSF.SetSceneStage(a[0], 2)   ; 0-based; also revives a just-ended scene
OSF.StopScene(a[0])
```

**Query current state:**
```papyrus
int stage = OSF.GetSceneStage(akActor)   ; -1 if not in a scene
```

---

## Player-participant scenes (compat locks)

The core never takes over the player automatically. If the player is a scene participant
and you want their body frozen (so the pinned rig doesn't yaw with the camera), engage the
standalone compat locks explicitly and release them when you stop:
```papyrus
OSFCompat.SetPlayerControlLock(true)    ; input-disable layer + AI-driven
OSFCompat.SetPlayerCameraLock(true)     ; force/hold third person
; ... run the scene ...
OSFCompat.SetPlayerControlLock(false)
OSFCompat.SetPlayerCameraLock(false)
```
These are the same mechanism the SAF shim uses. (Scene-integrated, automatic
control/camera/fade policy is OSF Intimacy.)

---

## Availability & save/load

**Check OSF and specific features before relying on them:**
```papyrus
If !OSF.IsReady()
    Return
EndIf
If OSF.HasFeature("scenes")           ; also "playback" / "sync" / "anchor"
    ; ...
EndIf
string ver = OSF.GetVersion()         ; "major.minor.patch"
```

**Save / load:** on a load OSF drops its own scene/graph state (it was anchored in the
discarded world) and re-binds its natives onto the rebuilt VM — nothing to do in the
common case. The core does **not** persist scene aftermath across saves and never resumes
playback from a save; replay from your own quest state if needed. (`OSF.NotifyGameLoaded()`
exists as a manual fallback; read its warning in `OSF.psc` before calling it.)

---

## Beyond the core — OSF Intimacy

Undress/redress, scheduled voice, camera/control takeover, fade-to-black choreography, and
scene/cue callbacks are provided by the **OSF Intimacy** scene engine layered on this core,
not by the core itself. Author intimate-scene orchestration against OSF Intimacy's
documentation.

**Authoring the animations themselves** (JSON packs, tags, stages, alignment offsets) is a
separate job — see [GETTING_STARTED.md](../GETTING_STARTED.md) and
[PACK_SCHEMA.md](../PACK_SCHEMA.md).
