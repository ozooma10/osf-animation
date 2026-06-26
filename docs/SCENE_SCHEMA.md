# OSF scene schema (`*.osf.json`)

OSF loads all content from `Data/OSF/**` at startup and again on `OSF.ReloadPacks()`. There is now
**one file kind** and **one content concept** — a **scene** — both plain JSON (`//` line comments are
allowed):

| File | Loaded by | Purpose |
|------|-----------|---------|
| `*.osf.json` | **SceneRegistry** | **Scenes** — id → roles + a clip timeline (linear) or a node graph, plus matchmaking and policy. |

All `*.osf.json` files are scanned recursively under `Data/OSF`. Bad files/entries are skipped and
reported via `OSF.GetSceneLoadErrors()` (and the `OSF Animation.log`). The current schema version is
**1** (`"schema": 1`).

A **scene** is the unified content entity an author writes (`SceneDef`). There is no separate "pack" or
"animation" content noun anymore — a clip is just a `.glb`/`.af` file, and the thing that sequences
clips is a scene. A *running* instance is also called a scene (a handle + anchor + participants + undo
ledger). `StartScene` starts an instance from a definition.

A scene is **minimal by default** (just clips) and **expands into graph features** (nodes, edges,
tracks, roles, policy) only when needed. By shape, not by type:

- **Linear** (the common case): a top-level `clip` or `stages[]`, no `nodes[]`. Desugars internally
  into a single auto-advancing node chain.
- **Graph**: top-level `nodes[]` (+ `entry`). The presence of `nodes[]` is the discriminator.

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
  "camera": "scene_orbit",               // pack-level default camera posture (default "scene_orbit"; "none" opts out)
  "scenes": [
    { "id": "author.one", "clip": "OSF/Anims/One.glb" },
    { "id": "author.two", "stages": [ { "loops": 0, "clips": ["OSF/Anims/Two.glb"] } ] }
  ]
}
```

- File-level `lockPlayer` / `stripActors` are optional **defaults** every scene in the file may
  override (the old pack file-default → per-entry override convenience).
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
- **Stage advance & the play-once default:** a stage advances when its `timer` elapses (`timer > 0`,
  seconds) or its clip has looped `loops` times (`loops > 0`). **A stage that specifies _neither_ plays
  through once and then advances** — so a multi-stage scene progresses linearly and the scene **ends
  after its final stage**. To make a stage hold a pose or loop forever (until a manual
  `SetSceneStage`/stop), give it an explicit **`"loops": 0`** (or `"timer": 0`). A single-stage looping
  idle therefore needs `"loops": 0`, or it will play once and end.
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
  { "name": "other", "races": ["Starfield.esm|0x0021A8D7"], "offset": { "y": 1.0, "heading": 180.0 } }
]
```

Each role is `{ name?, gender?, keywords?[], races?[], offset? }`:

- **`name`** is **OPTIONAL**. Omit it for an anonymous positional slot (`{}`); name it to bind via
  `StartSceneRoles` and to reference from track entries (`"role": "lead"`).
- **`gender`**: `"male"` | `"female"` | `"any"` (shorthand; or `"m"`/`"f"`).
- **`keywords`**: a form-ref string or array of them; the actor's base **or race** must carry any one.
- **`races`**: a form-ref string or array; the actor's race must equal any one.
- **`offset`**: the role's default placement for all stages.

A role's bound actor must satisfy **every present** constraint; within `keywords`/`races` the match is
**any-of**. **Form-ref format:** `"Plugin.esm|0xLOCAL"` (e.g. `"Starfield.esm|0x0021A8D7"`). Resolved
once at scene load; an unresolvable / wrong-type ref **rejects** the scene (see
`GetSceneValidationErrors`).

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
      "loop": { "mode": "once" },
      "edges": [ { "to": "main", "when": "end" } ] },

    { "id": "main",
      "stages": [ { "loops": 0, "clips": ["Main.glb", "Main2.glb"] } ],  // INLINE (the default)
      "loop": { "mode": "hold" },
      "cue":    [ { "at": 0.5,     "id": "beat" } ],                      // track lanes are flat keys
      "action": [ { "at": "enter", "type": "osf.fade.in" } ],
      "sound":  [ { "at": 0.5,     "spec": "event:Music", "role": "lead" } ],
      "camera": [ { "at": "enter", "state": "thirdperson_hold" } ],
      "edges": [
        { "id": "finish", "label": "Finish", "to": "climax", "when": "advance", "default": true },
        { "id": "tease",  "label": "Tease",  "to": "main",   "when": "advance" }    // self-loop
      ] },

    { "id": "climax", "use": "author.shared.peak", "loop": { "mode": "count", "count": 3 },
      "edges": [ { "to": "cooldown", "when": "loops" } ] },

    { "id": "cooldown", "use": "author.shared.winddown", "loop": { "mode": "once" } }
  ]
}
```

### Nodes

A node has **EXACTLY ONE playable**:

- **inline `stages[]`** — its own clip timeline (the default for one-offs), same shape as a linear
  scene's stages; **or**
- **`use: "<sceneId>"`** — reference another scene by id (see *Reuse* below).

Authoring **both, or neither**, is a hard load error.

```jsonc
{
  "id": "main",
  "stages": [ /* inline timeline */ ],   // OR "use": "<sceneId>"
  "loop": { "mode": "hold", "count": 0 },   // mode: "once" | "hold" | "count" (count uses "count")
  "loopForever": false,
  "timerSec": 0.0,                          // arms a node timer; pair with a {"when":"timer"} edge
  "edges": [ /* see below */ ],
  "cue": [], "action": [], "sound": [], "camera": []   // track lanes (flat keys)
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

### Reuse (`use`)

`use: "<sceneId>"` plays another scene's clips inside this node — the opt-in sharing path. Inline
`stages` is the default; `use` is for genuine cross-scene reuse.

- A `use` only splices the target's **entry node's stages**, so only a **single-node inline-stage
  scene** is a valid `use` target. A multi-way graph target is a load error.
- A **dangling `use`** (target id in no file) is a **load error**, surfaced via
  `OSF.GetSceneLoadErrors()`.

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
| `sound` | `{ "at", "spec", "role", "volume", "repeat" }` | `spec` is a Data-relative file or `"event:<name>"` Wwise spec; `role` positions it (else player). One **voice channel per actor** — see below. |
| `camera` | `{ "at", "state", "repeat" }` | `state` is a held camera posture (see below). Player-only (NPC scenes ignore it). |

#### Sound: one voice channel per actor

A sound plays on the **voice channel of its `role`'s actor** (the player's channel when no role
resolves). A channel plays **one sound at a time**: a new `sound`/`osf.voice.play` on an actor whose
channel is busy **replaces** (cuts) that actor's prior clip, so a `repeat:"loop"` vocal cue never
stacks over itself and a one-shot line cuts an ongoing loop. Different actors play independently. (The
miniaudio fallback cuts the prior clip outright today; the engine-native Wwise path tracks the prior
voice and cuts it once the AK stop entry is runtime-proven — until then a Wwise clip is tracked but
not yet cut.)

#### Camera `state` values

Camera postures are **held**: ledger-tracked and auto-restored to the player's prior POV on any scene
end. Supported states: `scene_orbit` (the default), `thirdperson_hold` (force and hold third person,
bouncing the player back if they zoom to first person), `freefly`, and `vanity_orbit`.

A pack with no `"camera"` key defaults to **`scene_orbit`** on each scene's entry node. Use
`"camera": "none"` at the file root to opt out and leave the player's camera untouched. An explicit
node-level `camera` track on the entry node always wins over the pack default.

---

## Policy

Set on a scene (or as a file-level default for `lockPlayer` / `stripActors`):

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

### Director input grant (`playerControl`)

`playerControl` is the per-scene player-input grant. Input is **enabled by default**: with no
`playerControl` block the player gets every capability while participating. A scene opts out wholesale
(`"playerControl": false`) or narrows it: `{ "disable": ["speed", "end"], "locked": true }`. Capabilities
are advance / navigate / speed / reposition / freecam / end; `locked: true` means the player may not end
the scene via the input channel (story scenes).

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
| `osf.weapon.sheathe` / `osf.weapon.restore` | Holster / re-draw the role's weapon. | ✓ | |
| `osf.fade.out` / `osf.fade.in` | Fade screen to/from black. | | `hold` (stay faded on cleanup), `duration` (ramp secs, 0 = default) |
| `osf.voice.play` | Play a sound spec positioned at the role. | ✓ | `set` (required: Data-relative path or `"event:<name>"`) |

> **Cleanup is automatic.** The ledger reverses control/camera/weapon/equipment/fade in reverse order
> on *every* end path (normal end, `StopScene`, interrupt, save-load) — none of the `*.osf.json`
> fixtures author a restore.

---

## Validation (at load)

Surfaced via `OSF.GetSceneLoadErrors()` (`[error]`/`[warn]` prefixed):

- A node with **both** `use` and `stages`, or **neither**, is a hard load error.
- A **dangling `use`** (target id in no file) is a load error.
- A `use` target that is **not a single-node inline-stage scene** is a load error.
- An authored id containing **`#`** is a load error (reserved for synthetic desugar nodes).
- A **duplicate id** within the one namespace → first-loaded-wins + a logged warning.

---

## Worked examples (shipped fixtures)

See `dist/OSF/` for runnable references:

- `OSFTestPack.osf.json` — a `{ "schema": 1, "scenes": [...] }` multi-scene file: solo, paired,
  multi-stage, timed, and loop-count **linear** scenes.
- `demo.osf.json` — a branching graph with **inline-stage** nodes and branchable `advance` edges
  (`finish` / `tease` self-loop).
- `autotest.osf.json` — a graph whose nodes **`use`** other scenes (`solo` / `solo.cover`) with
  `timerSec` + `timer`→`$end` auto-advance (the auto-end pattern).
- `soundtest.osf.json` — node-level `action` (`osf.voice.play`) + `sound` track lanes, including a
  `repeat:"loop"` numeric entry.
