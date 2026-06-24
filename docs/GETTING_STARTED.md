# Getting started with OSF

Two kinds of mod build on OSF:

1. **Content mods** — ship scenes as JSON (`*.osf.json`) + GLB. **No Papyrus, no scripting, no ESP
   required.** Drop files under `Data/OSF/**` and they're discovered.
2. **Trigger / consumer mods** — Papyrus (and usually an ESP) that decides *when* to start an OSF
   scene in response to gameplay. They call the [`OSF.*` API](API.md).

The split is deliberate: OSF is content-neutral, so animation **data** lives in JSON and gameplay
**policy** lives in your mod.

---

## 1. Ship a content mod (no scripting)

### a. Author the GLBs
Export your animation clips as GLB and place them anywhere Data-relative, conventionally
`Data/OSF/Animations/<YourPack>/*.glb`.

### b. Write a minimal scene (`Data/OSF/<yourscene>.osf.json`)
Everything you author is a **scene** (`*.osf.json`, `"schema": 2`). A minimal scene maps an **id** to
clip files + per-role placement + per-stage timing:

```jsonc
{
  "schema": 2,
  "id": "mypack.greet",
  "tags": ["social", "greet"],            // matchmaking tags
  "roles": [ {}, { "offset": { "y": 1.0, "heading": 180.0 } } ],  // optional; else inferred from clips
  "stages": [
    // No timer/loops -> the stage plays once and the scene ends. Add "loops": 0
    // to loop/hold this clip forever instead (e.g. an idle), or "timer"/"loops" to
    // auto-advance to a next stage.
    { "clips": [
      "OSF/Animations/MyPack/greet_a.glb",
      "OSF/Animations/MyPack/greet_b.glb"
    ] }
  ]
}
```

A solo or simple paired clip can stop here — `OSF.StartScene(actors, "mypack.greet")` or a tag query
will play it.

### c. (optional) Grow it into a graph scene
The **same** scene grows `nodes[]` (+ `entry`) when you want phases, branching, furniture anchoring, or
declarative immersion (camera/weapon/control/fade) with **automatic cleanup**. Each node plays an inline
`stages` timeline (the default) or `use`s another scene by id:

```jsonc
{
  "schema": 2,
  "id": "mypack.scenes.greet",
  "tags": ["social", "greet"],
  "roles": [ { "name": "lead", "gender": "any" }, { "name": "other", "gender": "any" } ],
  "entry": "main",
  "nodes": [
    {
      "id": "main",
      "stages": [ { "loops": 0, "clips": [          // inline; or "use": "mypack.greet"
        "OSF/Animations/MyPack/greet_a.glb",
        "OSF/Animations/MyPack/greet_b.glb"
      ] } ],
      "loop": { "mode": "hold" },
      "timerSec": 8.0,
      "camera": [ { "at": "enter", "state": "thirdperson_hold" } ],   // track lanes are flat keys
      "action": [ { "at": "enter", "type": "osf.control.lock", "role": "lead" } ],
      "edges": [ { "to": "$end", "when": "timer" } ]   // auto-end after 8s; ledger reverses camera+lock
    }
  ]
}
```

You author only the *engage* half of any `osf.*` mechanism — the undo ledger reverses it on every end
path. See [SCENE_SCHEMA.md](SCENE_SCHEMA.md) for the full field reference and the `osf.*` action list.

### d. Verify
```bat
cgf "OSFTest.SceneLoadTest"           ; reload + dump any load problems
cgf "OSFTest.IntrospectScene" "mypack.scenes.greet"
cgf "OSFTest.MatchTags" "greet"       ; matchmake on a tag (crosshair actor / player)
```
Iterate without restarting: edit the JSON/GLB, then `cgf "OSFTest.Reload"` (or `OSF.ReloadPacks()`).

---

## 2. Write a trigger mod (Papyrus)

A trigger mod listens to gameplay and starts an OSF scene. It holds **no animation data** — that's in
content scenes (yours or third-party). The pattern:

```papyrus
; On some gameplay event (a hit, a hotkey, sleeping near an NPC, ...):
Actor[] actors = new Actor[2]
actors[0] = akAnchorActor       ; actor 1 anchors the scene
actors[1] = akOtherActor

string[] allOf = new string[1]
allOf[0] = "takedown"
string[] anyOf = new string[0]  ; real empty arrays, never None
string[] noneOf = new string[0]

int handle = OSF.StartSceneByTagsQuery(actors, allOf, anyOf, noneOf)
If handle == 0
    ; no scene matched these actors + tags
EndIf
```

- **Matchmaking does the selection + validation.** Tag the scenes; let OSF pick by role/race/keyword/
  gender fit, priority tier, then weighted-random. You don't hand-roll a tag loop or actor-type check.
- **Anchoring at a thing** (a bed, a chair): set `SceneOptions.Anchor = akRef` and pass it as the
  last arg — `OSF.StartSceneByTags(actors, tags, opts)`.
- **Cleanup is automatic** via the ledger — to abort early just `OSF.StopSceneForActor(akActor)`.
- **React to lifecycle** with `OSF.RegisterSceneCallback` (see [API.md](API.md)).

### Worked example: the Starfield Interaction Framework (SIF)
SIF is exactly this shape. Its `SIF_PlayerEventHandler` (a player `ReferenceAlias`) listens for
`OnHit` / `OnCombatStateChanged` / `OnPlayerSleepStart`, applies a keyword-FormList fast-fail gate and
an RNG chance roll (gameplay policy), then calls `OSF.StartSceneByTagsQuery` /
`OSF.StartSceneByTags` (with `SceneOptions.Anchor` for the bed). Its `SIF_API` quest holds only the trigger→tag registries and branded custom
events. It ships zero animations — those are OSF JSON scenes. That is the whole division of
labour: SIF decides *when*; OSF does *everything else*.

---

## Build / install reference

See the top-level [README](../README.md) for building the plugin and the `XSE_SF_MODS_PATH` install
path. Content mods need no build step — they're just files under `Data/OSF/**`.
