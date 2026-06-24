# OSF content schema (packs & scenes)

OSF loads all content from `Data/OSF/**` at startup and again on `OSF.ReloadPacks()`. There are
two file kinds, both plain JSON (`//` line comments are allowed):

| File | Loaded by | Purpose |
|------|-----------|---------|
| `*.json` (not `*.scene.json`) | **PackRegistry** | **Animation packs** — id → per-actor clip files + placements + per-stage timing. |
| `*.scene.json` | **SceneRegistry** | **Scene graphs** — nodes/edges/roles/tracks that orchestrate pack animations. |

Both are scanned recursively under `Data/OSF`. Bad files/entries are skipped and reported via
`OSF.GetSceneLoadErrors()` (and the `OSF Animation.log`). The current schema version is **1** for
both kinds (`"schema": 1`).

A **pack animation** is the smallest playable unit (one or more actors, one or more stages of
clips). A **scene** is a graph that sequences pack animations and layers declarative policy
(camera/weapon/control/fade/sound, timers, branching, callbacks). Solo or simple paired clips can
be pure packs; anything with phases, immersion, or furniture anchoring is a scene.

---

## Animation packs (`*.json`)

```jsonc
{
  "schema": 1,
  "name": "My Pack",                         // diagnostics only
  "animations": [
    {
      "id": "author.pack.greet",             // unique id; referenced by scenes / StartScene / tags
      "name": "Friendly greeting",
      "tags": ["social", "greet"],           // free-form; used by StartSceneByTags* matchmaking
      "actors": [                            // OPTIONAL; one entry per participant slot.
        { "gender": "any" },                 //   gender: "male" | "female" | "any" (or "m"/"f")
        { "gender": "any", "offset": { "y": 1.0, "heading": 180.0 } }  // default placement for this slot
      ],                                     // omit entirely to infer the actor count from stages[].clips[]
      "stages": [                            // one or more stages; advance by timer/loops
        {
          "timer": 0.0,                      // seconds; 0 = no auto-advance on time
          "loops": 0,                        // clip loops before advancing; 0 = no auto-advance
          "clips": [                         // one per actor, index-aligned with "actors"
            "OSF/Animations/MyPack/greet_a.glb",
            { "file": "OSF/Animations/MyPack/greet_b.glb", "offset": { "y": 1.0, "heading": 180.0 } }
          ]
        },
        ["OSF/Animations/MyPack/greet2_a.glb", "OSF/Animations/MyPack/greet2_b.glb"]  // shorthand: bare clips array, default timer/loops
      ]
    }
  ]
}
```

- **`actors`** is optional. When present it sets the actor count and per-slot gender/offset defaults.
  When omitted, the actor count is inferred from the first stage's `clips[]`, and every actor defaults to
  gender `"any"` with no offset.
- **`clips`** entries are either a bare Data-relative path string, or `{ "file": ..., "offset": {...} }`
  to override that slot's placement for that stage. Every stage must have the same number of clips — equal
  to `actors.length` when `actors` is given, otherwise to the first stage's clip count.
- **Stage shorthand:** a stage may be written as a bare array of clips instead of a
  `{ timer, loops, clips }` object — e.g. `["a.glb", "b.glb"]` is exactly `{ "clips": ["a.glb", "b.glb"] }`
  with `timer`/`loops` defaulted to 0. The array entries are clips, so each may still be a bare path or a
  `{ "file", "offset" }` object. Mix shorthand and full-object stages freely within one `stages[]`.
- **`offset`** (a placement) corrects alignment relative to the scene anchor: `x`/`y`/`z` (local units)
  and `heading` (degrees). A slot-level `offset` is the default for all stages; a clip-level `offset`
  overrides it for that stage.
- **Clip paths are Data-relative** and load directly (they do **not** have to live under `Data/OSF` —
  e.g. `OSF/Animations/...` resolves to `Data/OSF/Animations/...`). Only *pack/scene JSON discovery* is
  restricted to `Data/OSF`.
- A multi-stage pack is a **linear scene**: `OSF.GetSceneStage`/`SetSceneStage` work on it by handle.

---

## Scenes (`*.scene.json`)

```jsonc
{
  "schema": 1,
  "id": "author.scenes.demo",
  "name": "Demo Scene",
  "priority": 0,                  // matchmaking tier (higher wins); ties broken by "weight"
  "weight": 1,                    // weighted-random sampling within the top priority tier
  "tags": ["paired", "demo"],     // free-form matchmaking tags
  "roles": [                      // declared participants, in binding order
    { "name": "lead",  "gender": "any" },
    { "name": "other", "filters": { "race": "Starfield.esm|0x0021A8D7" } }
  ],
  "entry": "approach",            // id of the node the scene starts on
  "nodes": [ /* see below */ ],
  "linearStages": ["approach", "main"]   // optional: stage index -> node id, for GetSceneStage/SetSceneStage
}
```

### Roles & filters
A role's bound actor must satisfy **every present** constraint; within `keyword`/`race` the match is
**any-of**.

- `gender`: `"male"` | `"female"` | `"any"` (shorthand; or `filters.gender`).
- `filters.keyword`: a form-ref string or array of them; the actor's base **or race** must carry any one.
- `filters.race`: a form-ref string or array; the actor's race must equal any one.
- **Form-ref format:** `"Plugin.esm|0xLOCAL"` (e.g. `"Starfield.esm|0x0021A8D7"`). Resolved once at scene
  load; an unresolvable / wrong-type ref **rejects** the scene (see `GetSceneValidationErrors`).

### Nodes
```jsonc
{
  "id": "main",
  "anim": "author.pack.main",       // pack animation id this node plays
  "loop": { "mode": "hold", "count": 0 },  // mode: "once" | "hold" | "count" (count uses "count")
  "loopForever": false,
  "timerSec": 0.0,                  // arms a node timer; pair with a {"when":"timer"} edge
  "edges": [ /* see below */ ],
  "tracks": { "cue": [], "action": [], "sound": [], "camera": [] }
}
```

- **`loop.mode`**: `once` (play through → `end` edge), `hold` (loop until advanced), `count` (loop
  `count` times → `loops` edge).
- **`timerSec`** only fires when the node also carries a `"when": "timer"` edge (a bare `timerSec` is a
  warning). This is the auto-advance / auto-end timer.

### Edges
```jsonc
{ "id": "finish", "label": "Finish", "labelKey": "", "to": "climax",
  "when": "advance", "trigger": "", "default": true, "priority": 0 }
```
- **`to`**: a node id in this scene, or `"$end"` to end the scene. (Edges cannot target another scene.)
- **`when`**: `end` (clip finished, `once`), `loops` (loop count reached, `count`), `timer` (node
  `timerSec` elapsed), `advance` (manual via `AdvanceScene`/`NavigateScene`), `trigger` (a cue fired —
  set `"trigger": "<cueId>"`).
- **Branchable** (`advance`) edges need `id` + `label` (for menus). `default: true` marks the edge
  `AdvanceScene` takes.

### Tracks (the four lanes)
Every track entry has a **position** (`at`) and optional **repeat**:
- **`at`**: a lifecycle anchor `"enter"` | `"exit"` | `"end"`, **or** a numeric **clip-fraction in
  `[0,1)`** (e.g. `0.6` = 60% through the clip). `at` is **not** wall-clock seconds.
- **`repeat`**: `"none"` (default) or `"loop"` (re-fire every clip loop). `repeat:"loop"` is only valid
  on numeric positions, not named anchors.

| Lane | Entry fields | Notes |
|------|--------------|-------|
| `cue` | `{ "at", "id", "repeat" }` | Fires `EVENT_CUE`; a `cue` id can drive a `trigger:<id>` edge. |
| `action` | `{ "at", "type", "role", "hold", "duration", "set", "repeat" }` | `osf.*` built-ins (below); any other namespace fires `EVENT_ACTION`. |
| `sound` | `{ "at", "sound", "role", "volume", "repeat" }` | `sound` is a Data-relative file or `"event:<name>"` Wwise spec; `role` positions it (else player). |
| `camera` | `{ "at", "state", "repeat" }` | `state` must be `"thirdperson_hold"` (only one supported; free-fly/orbit aren't yet). |

### `osf.*` action vocabulary (built-in mechanisms)
All are **recorded in the per-handle undo ledger and auto-reversed on any scene termination** — you do
**not** need to author the restore half.

| Action `type` | Effect | Needs `role` | Extra fields |
|---------------|--------|:---:|---|
| `osf.control.lock` / `osf.control.release` | Player input-disable + AI-driven lock (ref-counted). | ✓ | |
| `osf.equipment.hide` / `osf.equipment.restore` | Strip / restore the role's worn apparel (skin kept). | ✓ | |
| `osf.weapon.sheathe` / `osf.weapon.restore` | Holster / re-draw the role's weapon. | ✓ | |
| `osf.fade.out` / `osf.fade.in` | Fade screen to/from black. | | `hold` (stay faded on cleanup), `duration` (ramp secs, 0 = default) |
| `osf.voice.play` | Play a sound spec positioned at the role. | ✓ | `set` (required: Data-relative path or `"event:<name>"`) |

> **Cleanup is automatic.** The ledger reverses control/camera/weapon/equipment/fade in reverse order
> on *every* end path (normal end, `StopScene`, interrupt, save-load) — see `weapontest`/`equiptest`/
> `fadetest`/`cameratest` fixtures, none of which author a restore.

---

## Worked examples (shipped fixtures)

See `dist/OSF/` for runnable references:
- `OSFTestPack.json` — solo, paired, multi-stage, timed, loop-count packs.
- `demo.scene.json` — multi-node graph with branchable `advance` edges.
- `autotest.scene.json` — `timerSec` + `timer`→`$end` auto-advance (the auto-end pattern).
- `filtertest.scene.json` — a role `filters.race` form-ref.
- `weapontest` / `equiptest` / `fadetest` / `cameratest` / `soundtest` — one `osf.*` action lane each.
- `trigtest.scene.json` / `cuetest.scene.json` — cue tracks + `trigger:<cue>` edges.
