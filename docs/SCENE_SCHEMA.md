> WARNING: This file was AI-generated and is likely incorrect. Treat it as a rough draft, not authoritative documentation.

# OSF scene schema (`*.osf.json`)

OSF loads all content from `Data/OSF/**` at startup and again on `OSF.ReloadPacks()`. The primary content concept is a **scene** (`*.osf.json`); a secondary `*.sounds.json` file declares reusable **sound pools**.
Both are plain JSON (`//` line comments are allowed):

| File | Loaded by | Purpose |
|------|-----------|---------|
| `*.osf.json` | **SceneRegistry** | **Scenes** — id → roles + a clip timeline (linear) or a node graph, plus matchmaking and policy. |
| `*.sounds.json` | **SoundRegistry** | **Sound pools** — tagged, weighted clip sets a scene `sound`/`osf.voice.play` spec can draw from by tag (`$…`), optionally carrying subtitle text. See *Sound pools*. |

All `*.osf.json` files are scanned recursively under `Data/OSF`. Bad files/entries are skipped and
reported in `OSF Animation.log`. The current schema version is **1** (`"schema": 1`).

A **scene** is the unified content entity an author writes (`SceneDef`). There is no separate "pack" or
"animation" content noun anymore — a clip is just a `.glb`/`.af` file, and the thing that sequences
clips is a scene. A *running* instance is also called a scene (a handle + anchor + participants + undo
ledger). `StartScene` starts an instance from a definition.

A scene is **minimal by default** (just clips) and **expands into graph features** (branches, tracks,
roles, policy) only when needed. The two shapes share **one vocabulary** — a graph **node** is just a
linear **stage** that can also branch:

- **Linear** (the common case): a top-level `clip` or `stages[]`, no `nodes[]`. A stage is a clip
  timeline with `timer`/`loops` timing; stages auto-advance in order and the last one ends the scene.
  Desugars internally into a node chain.
- **Graph**: top-level `nodes[]` (+ `entry`). A node carries the **same** clips and the **same**
  `timer` / `loops` timing and `cue` / `action` / `sound` / `camera` lanes as a stage — it just adds
  `edges` (explicit transitions between nodes). The presence of `nodes[]` is the discriminator.

The keys never change meaning between the two forms: **`timer` is always seconds, `loops` is always the
clip-loop count** (`0` = hold, omit = play once). You learn the timeline vocabulary once on the linear
form and add `edges` only when you need branching.

---

## File layout

A file is either a **single bare scene object**, or an envelope with a `scenes[]` array:

```jsonc
// single-scene file
{ "schema": 1, "id": "author.wave", "clip": "OSF/Anims/Wave.glb" }
```

```jsonc
// multi-scene file
{
  "schema": 1,
  "name": "My Content",                  // diagnostics only
  "stripActors": true,                   // file-level default; each scene may override
  "lockPlayer": true,                    // file-level default; each scene may override
  "fade": false,                         // optional file-level start-curtain default; each scene may override
  "camera": "thirdperson_hold",          // file-level default camera posture (default "thirdperson_hold"; "none" opts out)
  "scenes": [
    { "id": "author.one", "clip": "OSF/Anims/One.glb" },
    { "id": "author.two", "stages": [ { "loops": 0, "clips": ["OSF/Anims/Two.glb"] } ] }
  ]
}
```

- File-level `lockPlayer` / `stripActors` / `fade` are optional **defaults** every scene in the file may
  override.
- Every scene needs a unique `id`. Within the one namespace, a duplicate id is **first-loaded-wins**
  plus a logged warning.
- Authored ids may **not** contain `#` (reserved for synthetic desugar nodes) — such an id is a load
  error.

---

## Linear scenes (`clip` / `stages[]`)

```jsonc
{
  "schema": 1,
  "id": "author.playthrough",
  "name": "Solo play-through",                 // diagnostics only
  "tags": ["solo"],                            // free-form matchmaking tags
  "priority": 0,                               // matchmaking tier (higher wins); ties broken by "weight"
  "weight": 1,                                 // weighted-random sampling within the top priority tier
  "lockPlayer": true,                          // disable player input while participating (false to opt out)
  "stripActors": true,                         // hide every participant's apparel (false to opt out)
  "fade": true,                                // opt into a start fade-to-black curtain when the player participates
  "roles": [                                   // OPTIONAL; else inferred from the first stage's clips
    {},
    { "offset": { "y": 1.0, "heading": 180.0 } }
  ],
  "stages": [                                  // one or more stages; advance by timer/loops
    { "clips": ["A.glb", "A.glb"] },                                       // play once, then advance
    { "timer": 6.0, "clips": ["B.glb", { "file": "B2.glb", "offset": { "y": 1.5 } }] },
    { "loops": 0,   "clips": ["C.glb", "C.glb"] }                          // hold; last stage ends the scene
  ]
}
```

- **`clip`** (string) is sugar for a single one-clip stage with one inferred role:
  `clip: "X.glb"` ≡ `roles: [{}], stages: [{ clips: ["X.glb"] }]`. A bare `clip` plays once, then ends.
- **`stages[]`**: one or more stages, each `{ timer?, loops?, clips[] }`.
- **`clips`** entries are either a bare Data-relative path string, or `{ "file": ..., "offset": {...} }`
  to override that role's placement for that stage. Every stage must have the same number of clips —
  equal to `roles.length` when `roles` is given, otherwise to the first stage's clip count. Clips
  **index-align** to role order.
- **Timing — the one timeline rule (linear _and_ graph).** A stage (linear) or a node (graph)
  specifies `timer` and/or `loops`; this table is the whole vocabulary, with identical meaning in both
  forms:

  | You write | Behaviour | Auto-advance when | (Graph) arm an edge with |
  |---|---|---|---|
  | *omit `timer` & `loops`* | play through once | the clip ends | `"when": "end"` |
  | `"loops": N`  (N ≥ 1) | loop N times | the Nth loop completes | `"when": "loops"` |
  | `"loops": 0` | **hold** — loop forever | never (manual advance/stop only) | `"when": "advance"` / `"timer"` / `"trigger:…"` |
  | `"timer": S` | run for S seconds | S seconds elapse | `"when": "timer"` |

  `timer` and `loops` may combine (whichever fires first wins). In a **linear** scene these conditions
  advance the stages automatically and the scene **ends after its final stage**; in a **graph** scene you
  wire the matching `edges` yourself (right column). A single-stage looping idle needs `"loops": 0`, or
  it plays once and ends.
- **Stage shorthand:** a stage may be written as a bare array of clips instead of a
  `{ timer, loops, clips }` object — e.g. `["a.glb", "b.glb"]` is exactly `{ "clips": ["a.glb", "b.glb"] }`
  (no timing, so it uses the play-once default). The array entries are clips, so each may still be a
  bare path or a `{ "file", "offset" }` object. Mix shorthand and full-object stages freely.
- **Track lanes on a stage:** a full-object stage may carry `cue`, `action`, `sound`, and `camera`
  lanes (same shape as on a node — see *Track lanes*); they run while that stage plays, forwarded
  onto the stage's desugared node. So a linear scene can fire cues, run actions, play sound, and hold
  a camera posture **without** dropping to the `nodes[]` graph form. The bare-array shorthand is
  clips-only — use the `{ … }` object form to attach a lane.
- **`offset`** (a placement) corrects alignment relative to the scene anchor: `x`/`y`/`z` (local units)
  and `heading` (degrees). A role-level `offset` is the default for all stages; a clip-level `offset`
  overrides it for that stage.
- **Clip paths are Data-relative** and load directly (they do **not** have to live under `Data/OSF` —
  e.g. `OSF/Animations/...` resolves to `Data/OSF/Animations/...`). Only *scene JSON discovery* is
  restricted to `Data/OSF`.
- **NAF compatibility shortcuts:** a clip spec starting `naf:` resolves under `Data/NAF`, and if `Data/<path>`
  is missing OSF also tries `Data/NAF/<path>`. A file or scene may set `"clipRoot": "NAF"` so bare clip paths
  in that scope are treated as NAF-relative.
- **GLB animation ids:** a clip object may include `{ "file": "NAF/Gangbang.glb", "anim": "COM.001" }`.
  The shorthand `"NAF/Gangbang.glb:COM.001"` is also accepted for `.glb`/`.gltf` paths.
- A multi-stage linear scene supports `OSF.GetSceneStage`/`SetSceneStage` (by handle) for manual
  stage jumps.

---

## Roles & filters

`roles[]` is the unified participant list (it replaces the old pack `actors` and scene `roles`). It is
**optional**: omit it and the actor count is inferred from the first stage's clips, with every role
defaulting to gender `"any"` and no offset.

```jsonc
"roles": [
  { "name": "lead",  "gender": "any" },
  { "name": "other", "filters": { "race": ["Starfield.esm|0x0021A8D7"] }, "offset": { "y": 1.0, "heading": 180.0 } }
]
```

Each role is `{ name?, gender?, filters?, offset?, equip? }`, where `filters` is `{ gender?, keyword?, race? }`:

- **`name`** is **OPTIONAL**. Omit it for an anonymous positional slot (`{}`); name it to bind via
  `StartSceneRoles` and to reference from track entries (`"role": "lead"`).
- **`gender`**: `"male"` | `"female"` | `"any"` (shorthand; or `"m"`/`"f"`). May be set directly on the
  role (as shown above) **or** nested as `filters.gender`.
- **`filters.keyword`**: a form-ref string or array of them; the actor's base **or race** must carry any
  one. **Note the singular key, nested under `filters`** (a bare top-level `keywords` is not read).
- **`filters.race`**: a form-ref string or array; the actor's race must equal any one. Singular, under
  `filters` (a bare top-level `races` is not read).
- **`offset`**: the role's default placement for all stages.
- **`equip`**: an item to equip onto this role's actor for the scene's duration, **auto-removed on
  every end path**. Either a bare form-ref string (any gender) or an object keyed by the actor's
  gender — `{ "male": ..., "female": ..., "any"?: ... }` (the bound actor's gender picks the ref;
  `any` is the fallback). If the actor didn't already own the item a copy is added and **destroyed on
  cleanup** (no inventory residue); a form the actor already wears is left untouched both ways.

  ```jsonc
  "roles": [
    { "name": "bottom" },
    { "name": "top", "equip": { "male": "Robert S Body Replacer.esm|0x804", "female": "Dick.esm|0x81D" } }
  ]
  ```

A role's bound actor must satisfy **every present** constraint; within `filters.keyword`/`filters.race`
the match is **any-of**. **Form-ref format:** `"Plugin.esm|0xLOCAL"` (e.g. `"Starfield.esm|0x0021A8D7"`).
`filters.keyword` / `filters.race` resolve **once at scene load** — an unresolvable / wrong-type ref
**rejects** the scene and is logged. `equip` refs resolve **at scene start** instead — one naming an
uninstalled plugin is **warned + skipped**, not a load error (they usually target optional body
replacers), so only the `"Plugin|0xLocal"` shape is checked at load.

### File-level roles

In a multi-scene file (`{ schema, "scenes": [ ... ] }`) a **file-level `roles`** block is a default:
every scene in `scenes` that omits its own `roles` inherits it (names, filters, offsets, **and
`equip`**). A scene that declares its own `roles` overrides the file-level roles entirely. (In a bare
single-scene file the top-level `roles` is simply that scene's roles.)

```jsonc
{
  "schema": 1,
  "roles": [ { "name": "bottom" }, { "name": "top", "equip": { "male": "...|0x804", "female": "...|0x81D" } } ],
  "scenes": [
    { "id": "author.scene.a", "stages": [ { "clips": ["A0.glb", "A1.glb"] } ] },   // inherits both roles + equip
    { "id": "author.scene.b", "stages": [ { "clips": ["B0.glb", "B1.glb"] } ] }
  ]
}
```

---

## Graph scenes (`nodes[]` + `entry`)

When a scene needs phases, branching, self-loops, cue-triggers, or per-node tracks, give it `nodes[]`
and an `entry` node id:

```jsonc
{
  "schema": 1,
  "id": "author.scenes.demo",
  "name": "Demo Scene",
  "priority": 5, "weight": 2,                    // matchmaking
  "tags": ["paired", "demo"],
  "lockPlayer": true, "stripActors": true,
  "playerControl": { "disable": ["speed"], "locked": true },
  "roles": [
    { "name": "lead",  "gender": "any" },
    { "name": "other", "gender": "female" }
  ],
  "entry": "approach",                           // id of the node the scene starts on
  "nodes": [
    { "id": "approach",
      "use": "author.shared.walkin",             // REUSE: play another scene by id
      // no `loops` -> play once, then take the `end` edge
      "edges": [ { "to": "main", "when": "end" } ] },

    { "id": "main",
      "stages": [ ["Main.glb", "Main2.glb"] ],   // INLINE timeline (bare-array stage = clips only)
      "loops": 0,                                // hold — same key/meaning as a linear stage
      "cue":    [ { "at": 0.5,     "id": "beat" } ],                     // track lanes are flat keys
      "action": [ { "at": "enter", "type": "osf.fade.in" } ],
      "sound":  [ { "at": 0.5,     "spec": "event:Music", "role": "lead" } ],
      "camera": [ { "at": "enter", "state": "thirdperson_hold" } ],
      "edges": [
        { "id": "finish", "label": "Finish", "to": "climax", "when": "advance", "default": true },
        { "id": "tease",  "label": "Tease",  "to": "main",   "when": "advance" }    // self-loop
      ] },

    { "id": "climax", "use": "author.shared.peak", "loops": 3,           // loop 3x -> take the `loops` edge
      "edges": [ { "to": "cooldown", "when": "loops" } ] },

    { "id": "cooldown", "use": "author.shared.winddown" }                // no `loops` -> play once -> ends
  ]
}
```

### Nodes

A node has **EXACTLY ONE playable**:

- **inline `stages[]`** — its own clip timeline (the default for one-offs), same shape as a linear
  scene's stages; **or**
- **`use: "<sceneId>"`** — reference another scene by id (see *Reuse* below).

Authoring **both, or neither**, is a hard load error. Beyond the playable, a node uses the **same
`timer` / `loops` timing keys as a linear stage** (the timing table under *Linear scenes* applies
verbatim) and adds `edges`:

```jsonc
{
  "id": "main",
  "stages": [ /* inline timeline */ ],   // OR "use": "<sceneId>"
  "loops": 0,                            // omit = once, 0 = hold, N = loop N  (same as a linear stage)
  "timer": 0.0,                          // seconds; arms a node timer — pair with a {"when":"timer"} edge
  "edges": [ /* see below */ ],
  "cue": [], "action": [], "sound": [], "camera": []   // track lanes (flat keys)
}
```

- **`loops`** is the node's loop count, identical to a linear stage: **omit** = play once (→ `end`
  edge), **`0`** = hold (loop until advanced), **`N`** = loop N times (→ `loops` edge). For a
  multi-stage inline node it bounds the **final** stage; earlier stages keep their own timing.
- **`timer`** (seconds) only fires when the node also carries a `"when": "timer"` edge (a bare `timer`
  is a warning). This is the auto-advance / auto-end timer.

### Edges

```jsonc
{ "id": "finish", "label": "Finish", "labelKey": "", "to": "climax",
  "when": "advance", "default": true, "priority": 0 }
```

- **`to`**: a node id in this scene, or `"$end"` to end the scene. (Edges cannot target another scene.)
- **`when`**: `end` (clip finished — the play-once case, `loops` omitted), `loops` (loop count reached,
  `loops: N`), `timer` (node `timer` elapsed), `advance` (manual via `AdvanceScene`/`NavigateScene`), or **`trigger:<cueId>`**
  (fires when that cue fires — the cue id is part of the `when` string, e.g. `"when": "trigger:beat"`;
  there is **no** separate `trigger` field, and a bare `"when": "trigger"` is a load error).
- **Branchable** (`advance`) edges need `id` + `label` (for menus). `default: true` marks the edge
  `AdvanceScene` takes.

### Reuse (`use`)

`use: "<sceneId>"` plays another scene's clips inside this node — the opt-in sharing path. Inline
`stages` is the default; `use` is for genuine cross-scene reuse.

- A `use` only splices the target's **entry node's stages**, so only a **single-node inline-stage
  scene** is a valid `use` target. A multi-way graph target is a load error.
- A **dangling `use`** (target id in no file) is a **load error** and is logged.

### Linear stage getters on a graph scene (`linearStages`)

A graph scene can opt into the linear stage API (`OSF.GetSceneStage`/`SetSceneStage`,
`GetSceneStageForActor`/`SetSceneStageForActor`) by listing the node ids that act as its sequential
"stages":

```jsonc
"linearStages": ["approach", "main", "climax"]
```

Each id must be a node in this scene; the list defines the stage-index ↔ node mapping the getters/setters
use. It's optional — only needed when a graph scene wants the linear stage controls.

---

## Track lanes (`cue` / `action` / `sound` / `camera`)

Track lanes are **flat keys** (`cue`, `action`, `sound`, `camera`) — there is no `tracks` wrapper
object. They attach to a graph **node** or, equally, to a linear **stage** (a stage's lanes are
forwarded onto its desugared node), so a linear scene gets the full lane vocabulary without `nodes[]`.
Every track entry has a **position** (`at`) and optional **repeat**:

- **`at`**: a lifecycle anchor `"enter"` | `"exit"` | `"end"`, **or** a numeric **clip-fraction in
  `[0,1)`** (e.g. `0.6` = 60% through the clip). `at` is **not** wall-clock seconds.
- **`repeat`**: `"none"` (default) or `"loop"` (re-fire every clip loop). `repeat:"loop"` is only valid
  on numeric positions, not named anchors.

| Lane | Entry fields | Notes |
|------|--------------|-------|
| `cue` | `{ "at", "id", "repeat" }` | Fires `EVENT_CUE`; a `cue` id can drive a `trigger:<id>` edge. |
| `action` | `{ "at", "type", "role", "hold", "duration", "set", "repeat" }` | `osf.*` built-ins (below); any other namespace fires `EVENT_ACTION`. |
| `sound` | `{ "at", "spec", "role", "volume", "repeat" }` — an **array/object `at`** makes it a **ladder** (see below) | `spec` is a Data-relative file or `"event:<name>"` Wwise spec (`spec` is canonical; `sound`/`pool` are accepted aliases); `role` positions it (else player). One **voice channel per actor** — see below. A clip can carry **subtitle text** (a spoken line) — see below. |
| `camera` | `{ "at", "state", "repeat" }` | `state` is a held camera posture (see below). Player-only (NPC scenes ignore it). |

#### Sound: one voice channel per actor

A sound plays on the **voice channel of its `role`'s actor** (the player's channel when no role
resolves). A channel plays **one sound at a time**: a new `sound`/`osf.voice.play` on an actor whose
channel is busy **replaces** (cuts) that actor's prior clip, so a `repeat:"loop"` vocal cue never
stacks over itself and a one-shot line cuts an ongoing loop. Different actors play independently. (The
engine-native Wwise path tracks each voice's `AkPlayingID` and cuts it via the AK stop
entry - `ExecuteActionOnPlayingID` - so the replace is an instant hard cut.)

#### Sound ladders: one lane fires at many positions

A `sound` entry's `at` is normally a single position. Make `at` an **array** (or a tag-keyed **object**)
instead and the entry becomes a **ladder** — one lane that fires at **many** positions. The lane's
`spec` / `role` / `volume` / `repeat` are shared defaults, and each hit appends its tag(s) to the base
`spec` (so a tag/pool spec picks an intensity-tagged variant per hit). The array/object `at` is exactly
what distinguishes a ladder from a flat entry (whose `at` is a scalar fraction or
`"enter"`/`"exit"`/`"end"`). Two shapes:

```jsonc
// GROUPED - the `at` object is keyed by the tag to append, value = the clip-fraction positions:
{ "spec": "event:Vocal", "role": "lead", "at": { "low": [0.1, 0.3], "loud": [0.8] } }

// ARRAY - ordered, heterogeneous; each entry is a bare position, [pos, "tag", …], or a per-hit object:
{ "spec": "event:Vocal", "role": "lead",
  "at": [ 0.2, [0.5, "loud"], { "at": 0.9, "tags": ["loud"], "volume": 1.2 } ] }
```

The lane's `at` carries the positions; inside a per-hit object the inner `at` is that one hit's position,
and a per-hit `spec` replaces the base.

#### Sound pools (`*.sounds.json`) and `$` specs

A `*.sounds.json` file (loaded by **SoundRegistry**, `"schema": 1`) declares reusable **sound pools** —
tagged, weighted clip sets a `sound` lane or `osf.voice.play` can draw from by tag instead of naming a file:

```jsonc
{ "schema": 1, "pools": [
  { "name": "seduce-f",                      // optional, diagnostics only
    "tags": ["seduce", "female", "moan"],    // REQUIRED; lowercased; how specs find this pool
    "clips": [                               // array (or the path -> text object form, see below)
      { "spec": "Sound/OSF/Seduce/F/a.wav", "weight": 2, "text": "Mmm..." },  // weight: sampling weight (default 1)
      "Sound/OSF/Seduce/F/b.wav"                                              // bare path = weight 1, no subtitle
    ] } ] }
```

A scene draws from a pool with a **`$`-prefixed, comma-separated tag spec** in any `sound`/`osf.voice.play`
`spec` — e.g. `"$seduce,{gender},moan"`. At **fire time** OSF substitutes `{gender}` (the role actor's
gender), finds the pool whose tags cover the query, and picks a clip **weighted-random** — so a looping or
repeated cue re-rolls each time. A plain path or `"event:<name>"` spec plays verbatim (no pool lookup). A
pool clip's path key is `spec` (canonical), or `file` as an alias.

#### Voice lines: text on a sound clip

A "voice" line is just a **sound clip that carries subtitle text**: when that clip plays — through any
path (the `sound` lane, `osf.voice.play`, or a `$pool` query) — the text shows in the dialogue box,
attributed to the actor the sound is positioned on. There is no separate lane: audio and the box are
the same clip. The text lives **with the clip in its `*.sounds.json` pool**, so authoring it once gives
that clip a subtitle everywhere it's used.

In a pool, the `clips` value may be the usual **array**, or — the shorthand for subtitled lines — an
**object mapping each clip path to its spoken text**:

```jsonc
// Data/OSF/seduce.sounds.json
{
  "schema": 1,
  "pools": [
    {
      "tags": ["seduce", "female"],
      // object form: path -> the line spoken when that clip plays
      "clips": {
        "Sound/OSF/Seduce/Female/00CCBA79.wav": "MMMMhhhmmm....",
        "Sound/OSF/Seduce/Female/00BD343F.wav": "This text is spoken by the actor"
      }
    },
    {
      "tags": ["seduce", "male"],
      // array form still works; add `text` to give an entry a subtitle, omit it for a silent clip
      "clips": [
        { "spec": "Sound/OSF/Seduce/Male/aa.wav", "text": "Come here.", "weight": 2 },
        "Sound/OSF/Seduce/Male/bb.wav"
      ]
    }
  ]
}
```

- The map **value** is the subtitle string (`null` / `""` = that clip plays with no box). The map **key**
  is the clip path; object-form clips have weight 1 (use the array `{ spec, weight, text }` form for a
  weight).
- Lookup is by the **final resolved spec**, so a `$pool` pick shows the picked clip's line and a direct
  path shows its own. A clip with no text just plays silently (no box) — nothing else changes.
- The same clip is still subject to the **one-voice-channel-per-actor** rule above, so a new line cuts
  that actor's previous one — line and box replace together.

> **Subtitle renderer.** Both halves are engine-native now. The **audio** rides Wwise; the **box** renders
> through the vanilla subtitle UI — `UI::Subtitle::Show` Notify()s the engine's `ShowSubtitleEvent`
> (AddrLib 86874, runtime-proven on 1.16.244, osf-re `ui.subtitle`), so the line shows in the standard
> bottom-of-screen list reading `speakerName: text` regardless of the user's subtitle settings. It is the
> shared list, **not** 3D-positioned on the speaker. The line is hidden (`HideSubtitleEvent`, 86875) once
> its hold elapses or on save-load teardown. If the event source can't resolve on a runtime, `Show` falls
> back to the HUD-message channel so a line is never lost. The authoring above is unchanged either way
> (`UI/Subtitle.cpp` is the one spot that knows how a line reaches the screen).

#### Camera `state` values

Camera postures are **held**: ledger-tracked and auto-restored to the player's prior POV on any scene
end. Supported states: `thirdperson_hold` (the default; force and hold third person, bouncing
the player back if they zoom to first person), `scene_orbit` (mouse-steered orbit), `freefly`, and `vanity_orbit`.

A file with no `"camera"` key defaults to **`thirdperson_hold`** on each scene's entry node. Use
`"camera": "none"` at the file root to opt out and leave the player's camera untouched. An explicit
node-level `camera` track on the entry node always wins over the file-level default.

**`thirdperson_hold` opening distance.** By default `thirdperson_hold` opens the camera **as far
zoomed out as the third-person axis allows**, so the scene doesn't start pinned on the player's back
when it forces third person from first person. The engine glides the camera out over ~1–2 s, and the
player can scroll-zoom freely afterward.

To open at a **specific** framing instead of fully out, author an optional numeric `distance`:

```json
"camera": [ { "at": "enter", "state": "thirdperson_hold", "distance": 1.5 } ]
```

`distance` is on the engine's **normalized third-person zoom axis `[0 .. 2]`** (not meters): ~`0` is
closest (pinned on the back), `2` is the farthest. It is **clamped** into range, and very small values
may cull the player's head, so prefer `~1.0` or higher. Omitting it (or `0`) means "fully out" — the
default. It is ignored by `freefly` / `vanity_orbit` / `scene_orbit` (those set their own framing). The
seed is applied per scene start and does not permanently change the player's own zoom.

---

## Policy

Set on a scene (or as a file-level default for `lockPlayer` / `stripActors` / `fade`):

### Player input lock (`lockPlayer`, default-on)

When the **player is a participant**, the scene engages the control lock (input-disable + AI-driven
decouple) automatically at start — you do **not** need to author `osf.control.lock`. It is
ledger-tracked, so it auto-releases on every end path.

- Set **`"lockPlayer": false`** to leave the player free (e.g. a scene they only spectate).
- The default never engages for an **NPC-only** scene (no player participant).
- Authored `osf.control.lock` / `osf.control.release` still work: the lock is idempotent (re-locking is
  a no-op), and an authored release can drop it mid-scene.

### Actor strip (`stripActors`, default-on)

At scene start the runtime hides **every participant's** worn apparel (the base skin/body is always
kept, so an actor is never made invisible) — you do **not** need to author `osf.equipment.hide`. It is
ledger-tracked, so each actor is re-dressed on every end path.

- Set **`"stripActors": false`** to keep actors clothed (then author per-role `osf.equipment.hide` for
  selective stripping).
- Unlike the player lock, this applies to **all** participants, including NPC-only scenes.
- Authored `osf.equipment.hide` / `osf.equipment.restore` still work.

### Screen fade (`fade`, default-off)

When **`"fade": true`** and the player is a participant, the scene posts a screen fade-to-black curtain
at start — a **self-releasing** fade (short ramp + bounded hold, then an automatic fade back in via the
per-frame tick). It is *not* ledger-held, so it un-fades a beat into the scene rather than staying
black until the end.

- Omit `fade` or set **`"fade": false`** to start without the curtain.
- Set **`"fade": true`** on a scene or at the file root to opt in for player-participant scenes.
- The default never engages for an **NPC-only** scene (the player's screen is never blacked out for a
  scene they're not in), and is a no-op where screen fades are unavailable on the runtime.
- **Caveat — this is a curtain, not a snap-hider.** The fade is posted to the UI queue (async) while the
  scene's actor teleport/strip/camera-cut run synchronously *this* frame, so the initial snap is already
  on screen before black arrives. The default gives a cinematic dip + settle, not a hidden start.
- Authored `osf.fade.out` / `osf.fade.in` still work (e.g. a held end-fade), independent of this default.

### Director input grant (`playerControl`)

`playerControl` is the per-scene player-input grant. Input is **enabled by default**: with no
`playerControl` block the player gets every capability while participating. A scene opts out wholesale
(`"playerControl": false`) or narrows it: `{ "disable": ["speed", "end"], "locked": true }`. Capabilities
are advance / navigate / speed / reposition / freecam / end; `locked: true` means the player may not end
the scene via the input channel (story scenes).

The object form also accepts **`"enabled": <bool>`** (an explicit on/off toggle — same effect as the
boolean `"playerControl": true|false` form) and **`"controlRole": "<roleName>"`** (advanced — names the
participant role the input grant is bound to; defaults to the player).

### Matchmaking (`tags`, `priority`, `weight`)

- **`tags[]`**: free-form matchmaking tags (matched by `StartSceneByTags*`).
- **`priority`** (int, default `0`): matchmaking tier — higher wins.
- **`weight`** (int, default `1`): weighted-random sampling within the top priority tier.

Matchmaking is over the one scene registry: a single query (tags + per-role gender/keyword/race fit)
returns the best candidate, ranked purely by `priority` tier then `weight`. A richer scene simply sets
a higher `priority` than a barer one.

---

## `osf.*` action vocabulary (built-in mechanisms)

All are **recorded in the per-handle undo ledger and auto-reversed on any scene termination** — you do
**not** need to author the restore half.

| Action `type` | Effect | Needs `role` | Extra fields |
|---------------|--------|:---:|---|
| `osf.control.lock` / `osf.control.release` | Player input-disable + AI-driven lock (ref-counted). **On by default when the player participates** — see *Player input lock*; author these only to override. | ✓ | |
| `osf.equipment.hide` / `osf.equipment.restore` | Strip / restore the role's worn apparel (skin kept). **All participants are stripped by default** — see *Actor strip*; author these only to override. | ✓ | |
| `osf.equipment.equip` / `osf.equipment.unequip` | Equip an arbitrary item on the role for the scene, then take it back off. A copy is added if the actor doesn't own one and **destroyed on cleanup** (no inventory residue); a form the actor already wears is left untouched both ways. | ✓ | `item` (required on `equip`: form ref `"<Plugin>\|0xLOCAL"`) |
| `osf.weapon.sheathe` / `osf.weapon.restore` | Holster / re-draw the role's weapon. | ✓ | |
| `osf.fade.out` / `osf.fade.in` | Fade screen to/from black. | | `hold` (stay faded on cleanup), `duration` (ramp secs, 0 = default) |
| `osf.voice.play` | Play a sound spec positioned at the role. If that clip carries subtitle text in its pool, the line shows in the box (see *Voice lines*). | ✓ | `set` (required: Data-relative path or `"event:<name>"`) |

> **Cleanup is automatic.** The ledger reverses control/camera/weapon/equipment/equipped-items/fade in
> reverse order on *every* end path (normal end, `StopScene`, interrupt, save-load) — you never
> author a restore.

---

## Validation (at load)

Surfaced in `OSF Animation.log`:

- A node with **both** `use` and `stages`, or **neither**, is a hard load error.
- A **dangling `use`** (target id in no file) is a load error.
- A `use` target that is **not a single-node inline-stage scene** is a load error.
- An authored id containing **`#`** is a load error (reserved for synthetic desugar nodes).
- A **duplicate id** within the one namespace → first-loaded-wins + a logged warning.

