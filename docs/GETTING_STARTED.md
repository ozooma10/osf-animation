# Getting started with OSF

Two kinds of mods build on OSF:

1. **Content mods** — ship scenes, routes, and/or curated animation entries as JSON (`*.osf.json`) plus GLB/AF assets. **No Papyrus, no scripting, no ESP required.** Drop JSON under `Data/OSF/**` and it is discovered.
2. **Trigger / consumer mods** — Papyrus that decides *when* to start an OSF scene in response to gameplay. They call the [`OSF.*` API](API.md).

The split is deliberate: OSF is content-neutral, so animation **data** lives in JSON and gameplay
**policy** lives in your mod.

Terms in this guide follow [OSF domain vocabulary](VOCABULARY.md). Prefer the canonical JSON keys
`hideApparel`, `playerInputLock`, and `sceneControls`; legacy `stripActors`, `lockPlayer`, and
`playerControl` spellings remain accepted for existing content.

---

## 1. Ship a content mod (no scripting)

### a. Author the clips
Export your animation clips as GLB or AF and place them anywhere Data-relative, conventionally under
`Data/OSF/Animations/<YourPack>/`.

### b. Register curated animations or poses (optional)

When the individual clips are the product—such as a static-pose pack—register them directly in the
Animations catalog with friendly names. No dummy scene is required:

```jsonc
{
  "schema": 1,
  "pack": "Moods of Andromas",
  "clipRoot": "OSF/Animations/MoodsOfAndromas",
  "folder": "Standing",
  "clipLibrary": [
    {
      "file": "arms-crossed.af",
      "name": "Arms Crossed",
      "folder": "Furniture/Seated"
    },
    "looking-away.af"                  // no name: browser falls back to the filename
  ]
}
```

Curated animations are one-actor, follow the actor's live placement, remain unlisted from
matchmaking, and hold until advanced or stopped. They appear even if no scene references them. See
the established `clipLibrary` key in
[SCENE_SCHEMA.md](SCENE_SCHEMA.md) for GLB animation ids, de-duplication, and mixed clip/scene files.
Use `folder` for the browser hierarchy under the pack; slash paths such as `Furniture/Seated`
create nested folders. A file-level folder is the default, and an object entry can override it.
Use `tags` only for optional qualities that should be searchable across folders.


### c. Write a minimal scene (`Data/OSF/<yourscene>.osf.json`)

A scene composes animation clip specs with roles, placement, timing, policy, and optional navigation.
A minimal scene maps an **id** to clip specs + per-role placement + per-linear-stage timing:

```jsonc
{
  "schema": 1,
  "id": "mypack.greet",
  "tags": ["social", "greet"],            // matchmaking tags
  "roles": [ {}, { "offset": { "y": 1.0, "heading": 180.0 } } ],  // optional; else inferred from clips
  "stages": [
    // No timer/loops -> the linear stage plays once and the scene completes. Add "loops": 0
    // to loop/hold this clip forever instead (e.g. an idle), or "timer"/"loops" to
    // auto-advance to the next linear stage.
    { "clips": [
      "OSF/Animations/MyPack/greet_a.glb",
      "OSF/Animations/MyPack/greet_b.glb"
    ] }
  ]
}
```

A solo or simple paired clip can stop here — `OSF.StartScene(actors, "mypack.greet")` or a tag query
will play it.

### d. (optional) Grow it into a graph scene
The **same** scene grows `nodes[]` (+ `entry`) when you want phases, branching, or declarative
immersion (camera/weapon/player-input-lock/fade) with **automatic cleanup**. (Furniture world
anchoring is separate and works for BOTH forms: add an inline `anchor` block — see SCENE_SCHEMA.md
§ Furniture world anchoring.) Each flow node plays an inline `stages` timeline (the default) or
`use`s another scene by id:

```jsonc
{
  "schema": 1,
  "id": "mypack.scenes.greet",
  "tags": ["social", "greet"],
  "roles": [ { "name": "lead", "gender": "any" }, { "name": "other", "gender": "any" } ],
  "entry": "main",
  "nodes": [
    {
      "id": "main",
      "stages": [ [                                 // inline timeline (bare-array stage = clips only)
        "OSF/Animations/MyPack/greet_a.glb",        // or "use": "mypack.greet"
        "OSF/Animations/MyPack/greet_b.glb"
      ] ],
      "loops": 0,                                   // hold (same key/meaning as a linear stage)
      "timer": 8.0,                                 // seconds; armed by the `timer` edge below
      "camera": [ { "at": "enter", "state": "thirdperson_hold" } ],   // track lanes are flat keys
      "action": [ { "at": "enter", "type": "osf.control.lock", "role": "lead" } ],
      "edges": [ { "to": "$end", "when": "timer" } ]   // auto-end after 8s; ledger reverses camera+lock
    }
  ]
}
```

You author only the *engage* half of any `osf.*` mechanism — the undo ledger reverses it after every
termination cause. See [SCENE_SCHEMA.md](SCENE_SCHEMA.md) for the full field reference and the
`osf.*` action list.

### e. (optional) Add an emote to the browser

A scene tagged `player.emote.<name>` automatically appears in the browser's Emotes group:

```jsonc
{
  "schema": 1,
  "id": "mypack.emote.salute",
  "tags": ["player.emote.salute"],
  "stages": [
    { "clips": ["OSF/Animations/MyPack/salute.glb"], "loops": 2 }   // must end on its own
  ]
}
```

The browser can launch the same scene on a selected NPC, so keep the role anonymous/unfiltered unless
the clip demands otherwise. Full contract: the well-known tags table in
[SCENE_SCHEMA.md](SCENE_SCHEMA.md).

### f. Verify
The shipped `OSF.*` natives are console-callable (the `OSFTest.*` harness is dev-only and does
NOT ship in the release):
```bat
cgf "OSF.ReloadPacks"        ; legacy name: rescan Data/OSF/** and reload content
cgf "OSF.OpenBrowser"        ; open the in-game browser (or press F10 / your bound hotkey)
```
Your pack groups under its `pack` name in the browser; launch it from there (the browser binds
the cast for you). Any `[error]` your file produced shows in OSF UI's System Health pane.
Iterate without restarting: edit the JSON/GLB, then `cgf "OSF.ReloadPacks"` again.

---

## 2. Write a trigger mod (Papyrus)

A trigger mod listens to gameplay and starts an OSF scene. It holds **no animation data** — that's in
content scenes (yours or third-party). The pattern:

```papyrus
; On some gameplay event (a hit, a hotkey, sleeping near an NPC, ...):
Actor[] actors = new Actor[2]
actors[0] = akAnchorActor       ; actor 1 supplies the default world anchor
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
- **World anchoring at a thing** (a bed, a chair): set `SceneOptions.Anchor = akRef` and pass it as the
  last arg — `OSF.StartSceneByTags(actors, tags, opts)`.
- **Cleanup is automatic** via the ledger — to cancel early just `OSF.StopSceneForActor(akActor)`.
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
