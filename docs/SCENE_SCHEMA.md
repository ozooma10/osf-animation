# OSF node schema (`*.osf.json`)

Animation 2.0 loads schema **2** files recursively below `Data/OSF`, at startup and on
`OSF.ReloadPacks()`. JSON comments are accepted. Invalid entries are diagnosed and skipped.
Sound pools remain separate `*.sounds.json` schema 1 documents.

A **node** is the registry entry: a global, case-insensitive ID, roles, one clip timeline, policy,
tracks and outgoing edges. A **scene** is the live handle owning participants, preparation and
cleanup. Its current node ID is its content address. A stage index is a playback segment inside
that node. There are no containing scene IDs, synthetic linear nodes or separate graph schema.
See [vocabulary](VOCABULARY.md) and [migration](V2_MIGRATION.md).

[Shared preparation](PREPARATION.md) gates starts, node transitions and internal stage changes.
Required equipment, resources, ownership and provider readiness must pass before playback commits.
Decorative props remain best effort; required attachments need a native readiness provider.

## File layout

Use a bare node for the shortest form:

```json
{ "schema": 2, "id": "author/wave", "clip": "OSF/Anims/Wave.glb" }
```

Or declare several nodes with file defaults:

```json
{
  "schema": 2,
  "pack": "My Content Pack",
  "tags": ["my-pack"],
  "hideApparel": false,
  "roles": [{"name": "player"}],
  "nodes": [
    {"id": "author/intro", "clip": "OSF/Anims/Intro.glb",
     "edges": [{"when": "end", "to": "author/idle"}]},
    {"id": "author/idle", "clip": "OSF/Anims/Idle.glb", "loops": 0}
  ]
}
```

A file may instead contain only `clipLibrary`, or combine it with `nodes`. Node metadata resolves
file defaults once at load. Scalar values and the role list replace defaults; tags form a stable,
case-insensitive union. `playerInputLock`, `hideApparel`, `clearHeldItems`, `fade`, `placement`,
`sceneControls`, camera posture, anchor, priority, weight and visibility apply to each resolved node.
Every node independently participates in matching: put an entry tag only on intended start nodes,
or select their exact IDs. Pack/folder are presentation groups, not address or inheritance scopes.

`section: "library"` selects reference content and defaults apparel hiding off. `unlisted` removes
a node from ordinary matchmaking. File-local role and prop templates are described below.
Each node needs a globally unique ID; duplicate IDs keep the first loaded definition and report
an error. Schema 1, nested `scenes`, `entry`, `linearStages`, node `use`, `allowManualHold`, and the
retired policy/attachment aliases are rejected. Prop-template `use` is still supported.

---

## Curated animations (`clipLibrary`)

Use the established `clipLibrary` key when the individual clips are the product—for example, a
static-pose pack. Each curated animation entry becomes a one-actor item under **Animations**, grouped
by the content file's `pack`. It is unlisted from matchmaking, follows the actor's live transform
without hiding apparel or engaging the player input lock, and holds until the user advances or stops
it. A clip-only file needs no dummy scene or `id`:

```jsonc
{
  "schema": 2,
  "pack": "Moods of Andromas",
  "clipRoot": "OSF/Animations/MoodsOfAndromas",
  "folder": "Standing",
  "clipLibrary": [
    {
      "file": "arms-crossed.af",
      "name": "Arms Crossed",
      "tags": ["arms-crossed"]
    },
    {
      "file": "contemplative.af",
      "name": "Contemplative",
      "folder": "Furniture/Seated"
    },
    "looking-away.af"
  ]
}
```

- An animation clip spec is a bare resource-path string, or the normal clip object
  `{ file, anim?, sec? }`, plus optional
  `name`, `folder`, and `tags` catalog metadata.
- `name` is the friendly browser label. When omitted, OSF falls back to the clip filename and appends
  the GLB animation id when present.
- `tags` are copied onto the curated animation for browsing/filtering. OSF also adds the internal
  `scene.clip` tag.
- File-level `clipRoot` applies exactly as it does to scene clips.
- `folder` organizes the browser within the pack. Use `/` for nesting, such as
  `Furniture/Seated`. It is presentation-only and never affects playback or matchmaking.
- A file-level `folder` is the default for every `clipLibrary` entry and for automatically exposed
  clips from scenes in that file. An object entry's `folder` overrides that default. With no folder,
  the item appears directly at the pack root.
- Folder paths are relative: no leading/trailing `/`, backslashes, empty segments, `.` or `..`.
  Display casing is preserved, while grouping is case-insensitive.
- Use folders for hierarchy and `tags` only for optional cross-cutting facets that may span folders.
  The pack already identifies the source mod, and `scene.clip` already identifies a raw clip.
- A curated animation appears even when no scene references it, and it appears for every player —
  unlike the entries the optional Browser addon derives from a scene's stages, which stay behind the
  browser's **Show author details** switch because they are a raw-clip inspection surface. The two share
  an id namespace. The catalog identifies an explicit registration with
  `sourceKind:"curatedAnimation"` and an automatic entry with
  `sourceKind:"derivedDebugAnimation"`. There is no legacy `curated` boolean.
  A pack that ships nothing but a `clipLibrary` is normal, complete content and works without Browser.
  Automatic inspection entries exist only with Browser installed; their generation and playback
  defaults belong to the addon. Core exposes generic clip-scene registration so these entries can
  still be started by ID through the ordinary native/Papyrus APIs after addon initialization.
- When a scene in the same catalog group (the declared content pack, or the source content file when
  no `pack` is declared) references the same file + animation id, OSF creates one
  curated animation and the explicit registration's `name`/`folder`/`tags` win over derived metadata.
- Duplicate explicit registrations for the same file + animation id in one such catalog group are a
  load error; the first entry is kept.
- A missing registered clip is reported and hidden from the Animations catalog.

`clipLibrary` and `nodes` may coexist:

```jsonc
{
  "schema": 2,
  "pack": "My Pack",
  "clipLibrary": [
    { "file": "OSF/Animations/MyPack/pose.glb", "name": "Heroic Pose" }
  ],
  "nodes": [
    { "id": "mypack.greeting", "clip": "OSF/Animations/MyPack/greeting.glb" }
  ]
}
```

Clips referenced by scenes in content files outside the reference catalog still receive automatic
filename-based inspection entries when no explicit registration supplies metadata.

---

## One node timeline (`clip` / `stages[]`)

Each `stages[]` entry is one playback segment. Public stage getters/setters use this zero-based
index. A timeline stays inside one node; it never generates additional node IDs.

```jsonc
{
  "schema": 2,
  "id": "author.playthrough",
  "name": "Solo play-through",                 // diagnostics only
  "tags": ["solo"],                            // free-form matchmaking tags
  "priority": 0,                               // matchmaking tier (higher wins); ties broken by "weight"
  "weight": 1,                                 // weighted-random sampling within the top priority tier
  "playerInputLock": true,                     // player input lock while participating (false to opt out)
  "hideApparel": true,                         // hide every participant's apparel
  "clearHeldItems": true,                      // temporarily unequip held weapons/items (false to opt out)
  "fade": true,                                // opt into a start fade-to-black curtain when the player participates
  "roles": [                                   // OPTIONAL; else inferred from the first stage's clips
    {},
    { "offset": { "y": 1.0, "heading": 180.0 } }
  ],
  "stages": [                                  // one or more stages; advance by timer/loops
    { "clips": ["A.glb", "A.glb"] },                                       // play once, then advance
    { "timer": 6.0, "clips": ["B.glb", { "file": "B2.glb", "offset": { "y": 1.5 } }] },
    { "loops": 0,   "clips": ["C.glb", "C.glb"] }                          // hold; final stage completes the scene
  ]
}
```

- **`clip`** (an animation clip spec string) is sugar for a single one-clip stage with one inferred role:
  `clip: "X.glb"` ≡ `roles: [{}], stages: [{ clips: ["X.glb"] }]`. A bare `clip` plays once, then completes.
- **`stages[]`**: one or more stages, each `{ timer?, loops?, hold?, clips[] }`.
- Holding belongs to caller policy. `SetSceneAutoAdvance(false)` suppresses automatic boundaries
  while sampling and cues continue. Explicit navigation and stop remain available.
- **`clips`** entries are animation clip specs: either a bare Data-relative resource-path string, or
  `{ "file": ..., "offset": {...} }`
  to override that role's placement for that stage. Every stage must have the same number of clips —
  equal to `roles.length` when `roles` is given, otherwise to the first stage's clip count. Clips
  **index-align** to role order. The JSON field `file` is the established serialization spelling; it
  is not proof that the resource has already been resolved or loaded.
- **Timing — the one timeline rule (linear _and_ graph).** A stage or a node
  specifies `timer` and/or `loops`; this table is the whole vocabulary, with identical meaning in both
  forms:

  | You write | Behaviour | Auto-advance when | (Graph) arm a scene edge with |
  |---|---|---|---|
  | *omit `timer` & `loops`* | play through once | the clip ends | `"when": "end"` |
  | `"loops": N`  (N ≥ 1) | loop N times | the Nth loop completes | `"when": "loops"` |
  | `"loops": 0` | **hold** — loop forever | never (manual advance/stop only) | `"when": "advance"` / `"timer"` / `"trigger:…"` |
  | `"loops": { ... }` (global binding below) | read the count at scene start | the resolved count completes; `0` holds | `"when": "loops"` |
  | `"timer": S` | run for S seconds | S seconds elapse | `"when": "timer"` |

  Stage timers and loop budgets advance through the timeline. Without node timing overrides,
  completing the last stage selects an `end` edge or ends the scene. Node `loops` overrides the
  terminal stage's budget and selects a `loops` edge; a node timer selects a `timer` edge.
  Earlier stages retain their own budgets. A single-stage idle needs `loops: 0` to hold.
  Final counted completion holds the last sampled frame during teardown and blend-out.
- **Global-backed loop counts (1.5.0).** A `loops` value can be an object with exactly two required
  fields: `global`, a `Plugin.esm|0xLocalID` reference to a game **GlobalVariable (GLOB)**, and
  `default`, an integer in `[0, 2147483647]`. For example:

  ```json
  {
    "id": "author.configurable",
    "stages": [
      {
        "loops": { "global": "Surrender.esm|0x1234", "default": 3 },
        "clips": ["OSF/Anims/Example.glb"]
      }
    ]
  }
  ```

  The reference uses the plugin's local hexadecimal form ID, just like role form references.
  A mod's menu or Papyrus script can change that global; OSF only reads it. No settings UI is
  created by this declaration, and no JSON is rewritten.

  OSF captures each referenced global once per destination admission. An active node retains
  those values; entering another node (including revisits) resolves that destination's globals.
  File reload is not needed to observe a changed global on the next admission.

  A missing plugin, missing form, wrong form type, or invalid global value uses the **local field's
  default**. Global values must be finite, non-negative whole numbers within the same integer
  range; fractional, negative, or oversized values fall back rather than truncate. Zero is valid
  and means hold until advanced (or until a timer/trigger ends the node). Missing globals do not
  reject the scene. Malformed declarations, references, or defaults do reject it at content load.

  This works on nodes and their stages. A graph node's own
  timing still controls its terminal stage; preceding inline stages retain their individual counts.
  For a global-backed graph node, use a `"when": "loops"` edge for counted completion, including
  when the default is zero. An advance edge lets a player/script leave a zero-count hold.
  Frozen `hold` frames cannot use `loops`, including this object form.

  `SceneOptions.LoopScale` is applied **after** resolving globals/defaults, rounds positive counts
  with a minimum of one, and leaves zero unchanged. Scaled counts saturate at `2147483647`.
  Catalog timing displays authored defaults; it does not read live globals. Older OSF versions
  do not accept the object form; ordinary integer `loops` remains supported.
- **`hold` — freeze on one frame instead of playing.** `"hold": true` parks the stage on its clips'
  **last** frame; `"hold": <0..1>` parks it on that normalized clip position (same axis as a track's
  `at`). The clock does not advance, so the pose is dead still and the clip never wraps:

  ```jsonc
  { "clips": ["OSF/Anims/HelmetOff.af"] },              // play the take-off once
  { "hold": true, "clips": ["OSF/Anims/HelmetOff.af"] } // then stand frozen on its final pose
  ```

  This is the way to author a **static hold state** — a "carrying it" or "waiting" beat between two
  animations. Freezing the *previous* stage's own last frame costs nothing at the seam: the stage
  change blends from a pose identical to the one it blends to, so there is no transition at all.
  Borrowing some other clip's idle instead re-poses the body to whatever that clip was authored
  against, which with a `mask` reads as the masked part popping to a different orientation.

  Because a frozen clip never loops, `hold` **cannot combine with `loops`** (the scene is rejected at
  load). `timer` still works and is the only automatic way out; otherwise the stage holds until a
  manual advance (Space / `AdvanceScene`) or a scene edge fires. Marks at or before the hold position fire
  once when the stage activates, so an `action`/`cue` at `at: 0` still runs; later marks never fire.
  Track lanes, `name`, and `tags` behave as on any other stage.
- **Linear-stage shorthand:** a stage may be written as a bare array of animation clip specs instead of a
  `{ timer, loops, clips }` object — e.g. `["a.glb", "b.glb"]` is exactly `{ "clips": ["a.glb", "b.glb"] }`
  (no timing, so it uses the play-once default). The array entries are clips, so each may still be a
  bare path or a `{ "file", "offset" }` object. Mix shorthand and full-object stages freely.
- **Tracks belong to the node.** Stage-scoped `cue`, `action`, `sound` and `camera` lanes are
  rejected. Use explicit nodes and edges when each segment needs its own tracks or policy boundary.
- **Stage metadata:** `name` and `tags` label a segment for browsing; node tags drive matchmaking.
  `SceneOptions.Stage` selects the initial segment. Browser single-stage preview ends at that
  segment's boundary rather than continuing to the next one.
- **Clip-level inspection in Animations.** Every distinct, installed clip referenced by a scene in a
  content file outside the reference catalog is also published automatically as a one-actor entry
  under **Animations**, grouped by the scene file's `pack` label (or by its `*.osf.json` filename when
  no pack is declared). Playing one runs only that raw clip and follows the actor's live transform.
  It neither hides apparel nor engages the player input lock, and it does not apply fade, role offset,
  or scene tracks.
  Generated vanilla/reference-catalog scenes and emotes are excluded because they already populate
  Animations.
- **`offset`** (a placement) corrects alignment relative to the scene's world anchor: `x`/`y`/`z` (local units)
  and `heading` (degrees). Offsets rotate with the actor's rendered basis: positive heading is
  clockwise viewed from above, so local `+Y` becomes world `+X` at a 90-degree anchor heading.
  A role-level `offset` is the default for all stages; a clip-level `offset`
  overrides it for that stage.
- **Clip paths are Data-relative** and load directly (they do **not** have to live under `Data/OSF` —
  e.g. `OSF/Animations/...` resolves to `Data/OSF/Animations/...`). Only *content JSON discovery* is
  restricted to `Data/OSF`.
- **NAF compatibility shortcuts:** an animation clip spec starting `naf:` resolves under `Data/NAF`, and if `Data/<path>`
  is missing OSF also tries `Data/NAF/<path>`. A content file or scene may set `"clipRoot": "NAF"` so bare clip paths
  in that scope are treated as NAF-relative.
- **GLB animation ids:** a clip object may include `{ "file": "NAF/Gangbang.glb", "anim": "COM.001" }`.
  The shorthand `"NAF/Gangbang.glb:COM.001"` is also accepted for `.glb`/`.gltf` paths.
- A multi-stage node timeline supports `OSF.GetSceneStage`/`SetSceneStage` (by handle) for manual
  stage jumps.

---

## Roles & filters

`roles[]` is the authored role list. It replaces the old pack `actors` and earlier scene-role forms,
but it is not itself the live participant roster. The caller's **cast** is bound to these roles at
scene start; the bound actors then become the running scene's **participants**. `roles[]` is
**optional**: omit it and the actor count is inferred from the first stage's clips, with every role
defaulting to gender `"any"` and no offset. Every node infers from its own first stage when no default or explicit role list is supplied.

```jsonc
"roles": [
  { "name": "lead",  "gender": "any" },
  { "name": "other", "filters": { "race": ["Starfield.esm|0x0000347D"] }, "offset": { "y": 1.0, "heading": 180.0 } }
]
```

Each role is `{ name?, gender?, filters?, poseMode?, poseWeight?, mask?, preserveBones?, offset?, equip? }`, where `filters` is `{ gender?, keyword?, race? }`.
(A `roles` entry may also reference the file-level roles registry — a plain id **string**, or an
`{ "id": ..., ...overrides }` object; see *File-level roles* below.)

- **`name`** is **OPTIONAL**. Omit it for an anonymous positional role (`{}`); name it to bind via
  `StartSceneRoles` and to reference from track entries (`"role": "lead"`).
- **`gender`**: `"male"` | `"female"` | `"any"` (shorthand; or `"m"`/`"f"`). May be set directly on the
  role (as shown above) **or** nested as `filters.gender`.
- **`filters.keyword`**: a form-ref string or array of them; the actor's base **or race** must carry any
  one. **Note the singular key, nested under `filters`** (a bare top-level `keywords` is not read).
- **`filters.race`**: a form-ref string or array; the actor's race must equal any one. Singular, under
  `filters` (a bare top-level `races` is not read).
- **`poseMode`**: `"override"` (the default) or `"additive"`. Override is the historical behavior:
  OSF writes the clip's sampled local pose on every driven bone. Additive treats the sample as a
  gesture layered over Starfield's current locomotion/stance pose. For each joint, the reference is
  that active animation skeleton's **local-to-parent rest transform** (`joint_rest_poses`, in the same
  space as the sampled track): rotation delta is `inverse(reference) * sample`, translation delta is
  `sample - reference`, and the weighted delta is composed as `live * delta`. A stage change loads
  both its new sample and its new skeleton reference. Additive scale is intentionally ignored, so the
  engine's live scale stays authoritative.
- **`poseWeight`**: optional persistent layer strength, default `1.0`. It must be a finite JSON number
  and is clamped to `[0, 1]`. It applies in **both** pose modes: additive scales the delta, override
  blends the sampled pose against the engine's live pose per bone. During blend-in and blend-out the
  effective strength is the transition weight multiplied by `poseWeight`; `blendIn` remains transition
  **time**, not layer strength. At weight zero OSF leaves the live pose unchanged.
- **`mask`**: optional named driven-bone whitelist for partial-body playback. With a mask the role
  drives ONLY the mask's rig bones — every other bone stays fully engine-driven, so a gesture (an
  equip, a wave, a held prop) plays **over** Starfield's own locomotion instead of replacing it (the
  behavior-graph "upper body while moving" look). Weight-1 bones take the sampled pose absolutely;
  fractional bones blend sampled-vs-live per bone every frame. Named masks (human rig):

  | mask | covers |
  |---|---|
  | `"upperBody"` | chest/neck/head, both arms, weapon bones at full weight, over a feathered spine seam (`C_Spine` 0.25, `C_Spine1` 0.5, `C_Spine2` 0.75) so run/walk torso sway survives underneath |
  | `"arms"` | both clavicle-down arm chains + weapon bones |
  | `"leftArm"` / `"rightArm"` | one arm chain + its weapon bone |
  | `"lowerBody"` | hips/waist/legs/feet (COM stays engine-driven, so root motion is untouched) |
  | `"hands"` | both wrists + fingers + in-hand prop helpers |

  The mask name is matched case-insensitively and validated at load (an unknown name rejects the
  scene). Interplay: `preserveBones` still hard-excludes bones from a mask; in `"additive"` mode the
  mask scales each bone's delta instead. Bone names match live rig nodes case-insensitively — bones a
  species rig doesn't have simply never match, so a mask on an off-species rig degrades toward
  stamping nothing. `Eye/face` nodes remain engine-driven as always. Typical equip-style gesture:

  ```jsonc
  { "name": "gesturer", "mask": "upperBody" }
  ```

  An object-form stage clip may override that role default for only that stage:

  ```jsonc
  { "clips": [{ "file": "OSF/Anims/HelmetHold.af", "mask": "rightArm" }] }
  ```

  The override is validated like a role mask and reverts to the role's mask on the next stage.

- **`preserveBones`**: an array of exact, case-insensitive rig bone names that remain engine-driven
  for this role. It is a hard write exclusion in both pose modes: OSF omits only those bone slots
  while continuing to animate independently bound child bones. This is useful when a baked full-pose
  clip contains an unwanted transform for a helper/root bone, without making the role gender-specific:

  ```jsonc
  { "name": "receiver", "preserveBones": ["C_GenitalsRoot"] }
  ```

  An additive full-body clip still contributes reference-relative deltas for every bound body joint;
  additive composition does not infer an arm mask from the motion itself. For a gesture that should
  only affect part of the body, use `mask` (the driven-bone whitelist) rather than enumerating
  everything else in `preserveBones`.

- **`offset`**: the role's default placement for all stages.
- **`equip`**: an item to equip onto this role's actor for the scene's duration, **auto-removed on
  every termination path**. Either a bare form-ref string (any gender) or an object keyed by the actor's
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
the match is **any-of**. **Form-ref format:** `"Plugin.esm|0xLOCAL"` (e.g. `"Starfield.esm|0x0000347D"`).
`filters.keyword` / `filters.race` resolve **once at scene load** — an unresolvable / wrong-type ref
**rejects** the scene and is logged. `equip` refs resolve **at scene start** instead — one naming an
uninstalled plugin is **warned + skipped**, not a load error (they usually target optional body
replacers), so only the `"Plugin|0xLocal"` shape is checked at load.

### File-level roles

In a multi-scene file (`{ schema, "nodes": [ ... ] }`) the **file-level `roles`** key is read by its
JSON type:

- an **array** is a **default role list**;
- an **object** is a **roles registry** of reusable, referenceable definitions.

(In a bare single-scene file the top-level `roles` is simply that scene's roles, and must be an array.)

#### Default role list (array)

Every scene in `scenes` that omits its own `roles` inherits the complete array (including pose policy,
filters, offsets, and `equip`). A scene that declares its own `roles` overrides the file-level roles
entirely.

```jsonc
{
  "schema": 2,
  "roles": [ { "name": "bottom" }, { "name": "top", "equip": { "male": "...|0x804", "female": "...|0x81D" } } ],
  "nodes": [
    { "id": "author.scene.a", "stages": [ { "clips": ["A0.glb", "A1.glb"] } ] },   // inherits both roles + equip
    { "id": "author.scene.b", "stages": [ { "clips": ["B0.glb", "B1.glb"] } ] }
  ]
}
```

#### Roles registry (object)

An object maps an **exact, case-sensitive id** to a reusable role **template**. Scenes instantiate a
template inside their own `roles` array in two forms, freely mixed with inline role objects:

- a **string** — the bare id — copies the template as-is;
- an **object containing `id`** — copies the template, then applies the entry's remaining keys as
  overrides (the `id` key itself is consumed by the reference, not parsed as role data).

Both expand to ordinary roles at load, so everything downstream (matchmaking, `StartSceneRoles`,
track `"role"` refs) works exactly as with inline roles.

```jsonc
{
  "schema": 2,
  "roles": {
    "m":  { "gender": "male", "equip": { "male": "...|0x804", "female": "...|0x81D" } },
    "f":  {}                                                     // base name defaults to the id: "f"
  },
  "nodes": [
    // MMF — the repeated template is auto-numbered: runtime names "m", "m2", "f".
    { "id": "author.scene.mmf", "roles": ["m", "m", "f"], "stages": [ { "clips": ["A0.glb", "A1.glb", "A2.glb"] } ] },
    // A named instance (e.g. a track target: "role": "lead") with a per-role offset override.
    { "id": "author.scene.duo",
      "roles": [ { "id": "m", "name": "lead", "offset": { "y": 1.0 } }, "f" ],
      "stages": [ { "clips": ["B0.glb", "B1.glb"] } ] },
    { "id": "author.scene.c", "stages": [ { "clips": ["C0.glb", "C1.glb"] } ] }   // no roles: normal inference
  ]
}
```

**Overrides** merge onto the template JSON-merge-patch style:

- **Scalars replace** — `"gender": "female"` replaces the template's `gender`; `poseMode`,
  `poseWeight`, and `mask` replace their inherited scalar values the same way.
- **`filters`, `offset`, and `equip` merge by key** — `{ "offset": { "y": 1.0 } }` changes only `y`;
  `{ "equip": { "female": "..." } }` changes only the `female` ref. Unspecified keys are inherited.
- **Arrays replace wholesale** — a `preserveBones` override substitutes the whole list.
- **`null` removes an inherited optional field** — `"equip": null` instantiates the template without
  its equip.
- `gender` and `filters.gender` are **aliases** for one constraint: overriding either drops the
  inherited value on the other path, so the merged role can never trip the "they disagree" check.
  (Supplying both in one override is validated exactly like an inline role.)

**Runtime names** are assigned deterministically:

- A template that omits `name` takes its **registry id** as its base name; an explicit `name: ""`
  (on the template or an override) stays an **anonymous positional role**.
- A **string** reference — or `{ "id": ... }` with no `name` — gets an **automatic** name: the
  template's effective name, suffixed `2`, `3`, … past any name already used in the scene. Three
  males and a female are simply `["m", "m", "m", "f"]` → `m`, `m2`, `m3`, `f`.
- An **explicit** `name` (on an override object or an inline role) is kept **exactly** and reserved
  first, so automatic names skip it: `["m", { "id": "m", "name": "m" }]` → `m2`, `m`. **Duplicate
  explicit names reject the scene** (case-insensitive).
- Anonymous roles are never numbered.

**Validation:**

- A malformed **template** (non-object value, or a role object that would be rejected inline)
  rejects the **whole file** at load.
- A scene with an **unknown, empty, or non-string `id`** (ids are case-sensitive), or with
  **duplicate explicit role names**, rejects **only that scene**; diagnostics name the source file,
  scene id, and role id.
- **File-local, templates-only:** the registry is visible only to the scenes in its own file. There
  are no aliases, cross-file references, or whole-role-set references.
- **Not a default role list:** a scene that omits `roles` under a registry gets the usual clip-count
  inference (anonymous roles), never the registry contents.

---

## Routes over node IDs (`edges[]`)

Every node supplies exactly one source, `clip` or `stages`. A route is just edges over global IDs;
it may cross file and pack boundaries. Start at any node by its ID. The original publication stays
pinned while following authored edges, even after reload/provider removal. Exact caller transitions
resolve the current publication. Both use the same admission and preparation executor.

```json
{
  "id": "author/idle",
  "stages": [{"clips": ["Main.glb", "Main2.glb"]}],
  "loops": 0,
  "cue": [{"at": 0.5, "id": "beat"}],
  "edges": [
    {"id": "finish", "label": "Finish", "to": "author/ending", "when": "advance", "default": true},
    {"id": "change", "label": "Change pose", "to": "other/pose", "when": "advance", "covered": true}
  ]
}
```

Edges have `to`, `when`, optional `id`, `label`, `labelKey`, `default`, `priority`, and `covered`.
`to` names a global node or `$end`. `when` is `end`, `loops`, `timer`, `advance`, or `trigger:<cueId>`.
Branchable `advance` edges need an ID and label. `default: true` selects the manual Advance edge;
priority and declaration order resolve competing eligible edges. Within a multi-stage timeline,
Advance first advances the current stage; named navigation selects its edge directly.

`covered` defaults false. True requests the shared exclusive cover/deadline/stamped-pose reveal
mechanism; unavailable cover is reported through preparation. It changes the boundary's cost,
not its destination semantics. A manual refusal before mutation may retain the source; a consumed
automatic boundary terminates on refusal. See [shared preparation](PREPARATION.md).

Node `loops` bounds the final stage; earlier stages retain their timing. Node `timer` needs a timer
edge. Without node timing overrides, the complete timeline's final boundary uses an `end` edge or
terminates. Missing destinations reject the source and its dependent routes in the completed
publication, including generated/provider entries; valid cycles remain available.

---

## Track lanes (`cue` / `action` / `sound` / `camera`)

Track lanes are **flat keys** (`cue`, `action`, `sound`, `camera`) — there is no `tracks` wrapper
object. They attach to a node and run while its timeline plays. Every entry has a **track position**
(`at` **or** `atFrame`) and optional **repeat**:

- **`at`**: a named track position `"enter"` | `"exit"` | `"end"`, **or** a numeric **clip-fraction in
  `[0,1)`** (e.g. `0.6` = 60% through the clip). `at` is **not** wall-clock seconds.
- **`atFrame`**: a **zero-based clip frame** at 30 fps (e.g. `24` = 0.8 s into the clip). Use it
  whenever you picked the moment in an animation tool — no dividing by the clip length. See
  [Frames vs. fractions](#frames-vs-fractions) below.
- **`repeat`**: `"none"` (default) or `"loop"` (re-fire every clip loop). `repeat:"loop"` is only valid
  on numeric positions, not named track positions.

`at` and `atFrame` are mutually exclusive — an entry that sets both is rejected.

| Lane | Entry fields | Notes |
|------|--------------|-------|
| `cue` | `{ "at"\|"atFrame", "id", "role", "repeat", "delivery" }` | Fires `EVENT_CUE`; a `cue` id can drive a `trigger:<id>` scene edge. Optional `role` names a declared role: the event's `role` is set and `actorRef` (native `actor`) is that role's bound participant. An undeclared role is a load error. |
| `action` | `{ "at"\|"atFrame", "type", "role", "emitter", "hold", "duration", "set", "item", "prop", "use", "source", "attachmentNode", "position", "rotation", "scale", "repeat" }` | `osf.*` built-ins (below); any other namespace fires `EVENT_ACTION`. Fields are mechanism-specific. For prop attach, `attachmentNode` names the actor **attachment node**, not a node; legacy alias `node` remains accepted. |
| `sound` | `{ "at"\|"atFrame", "spec", "role", "emitter", "repeat" }` — an **array/object** position makes it a **ladder** (see below) | `spec` is a Data-relative file or `"event:<name>"` Wwise spec (`spec` is canonical; `sound`/`pool` are accepted aliases). `role` selects the actor's voice channel, gender substitution, and subtitle speaker. `emitter` is `"listener"` (default) or `"role"` for world-positioned audio that follows that actor. A clip can carry **subtitle text** (a spoken line) — see below. |
| `camera` | `{ "at"\|"atFrame", "state", "repeat" }` | `state` is a held camera posture (see below). Player-only (NPC scenes ignore it). |

#### Cue delivery

The optional cue field `delivery` accepts `"both"` (the default) or `"native"`.
Native-only cues invoke synchronous native scene callbacks, then skip Papyrus target
collection, struct allocation, and VM scheduling. Use this for high-frequency cues
whose consumers are entirely native. Ordinary cues and scene/node lifecycle events
retain their existing delivery. Unknown values and non-string values reject the scene.

This applies to graph and compiled-stage lanes, frame/fraction marks (including loop
repeats), and enter/exit/end cues. Native payloads, role binding, original detection
timestamps, and `trigger:<id>` edges are unchanged. Delivery selects subscribers; it
does not alter cue timing or navigation. An older engine that ignores this field will
still deliver to Papyrus, so content requiring suppression needs this implementation.

```json
{ "atFrame": 24, "id": "beat", "role": "lead", "repeat": "loop", "delivery": "native" }
```

#### Frames vs. fractions

```jsonc
"cue":    [ { "atFrame": 24, "id": "beat" } ],          // frame 24 = 0.8 s in
"action": [ { "atFrame": 0,  "type": "osf.fade.in" } ]  // frame 0 = the clip start
```

`atFrame` must be a **whole number ≥ 0**. Frames are counted at **30 fps** — the rate an `.af`
decodes at — so
`atFrame: N` is exactly `N / 30` **clip-local seconds**.

That makes the two positions behave differently when the clip length changes:

| | `at: 0.5` | `atFrame: 15` |
|---|---|---|
| 1 s clip | fires at 0.5 s | fires at 0.5 s |
| 2 s clip | fires at 1.0 s (**slides**) | fires at 0.5 s (**stays**) |

So a frame keeps the moment you authored even if the clip is re-exported longer, while a fraction
re-scales with it. The flip side: a frame **past the end** of the clip it plays on has nowhere to
land and simply never fires (there is no clip length at load time to validate it against, so this is
not a load error — check the frame against your clip). The browser's track rail flags such a mark
as **PAST END** (dashed marker) and the inspector skips it, matching the runtime. Use
`"at": "end"` for the clip's final frame.

`repeat: "loop"` works the same for both: the position is measured from the start of **each** loop.

#### Sound: one voice channel per actor

A sound plays on the **voice channel of its `role`'s actor** (the player's channel when no role
resolves). A channel plays **one sound at a time**: a new `sound`/`osf.voice.play` on an actor whose
channel is busy **replaces** (cuts) that actor's prior clip, so a `repeat:"loop"` vocal cue never
stacks over itself and a one-shot line cuts an ongoing loop. Different actors play independently. (The
engine-native Wwise path tracks each voice's `AkPlayingID` and cuts it via the AK stop
entry - `ExecuteActionOnPlayingID` - so the replace is an instant hard cut.)

#### Sound emitters: listener or role actor

Sound remains listener-relative by default for compatibility. `"emitter":"role"` requests a
positioned Wwise game object associated with the actor selected by `role`:

```jsonc
{ "at": 0.35, "spec": "Sound/OSF/MyPack/voice01.wem",
  "role": "other", "emitter": "role" }
```

If the role has no live actor, or no verified engine-owned Wwise object is available, OSF falls back
to the listener. The current Starfield 1.16.244 backend takes this fallback: the former guessed
registration entry point was removed rather than invoking an unverified ABI. The emitter controls
where the audio is heard; `role` still owns
voice-channel replacement and subtitle attribution. The Wwise event controls the actual attenuation
and spatialization curve.

#### Sound ladders: one lane fires at many positions

A `sound` entry's `at` is normally a single position. Make `at` an **array** (or a tag-keyed **object**)
instead and the entry becomes a **ladder** — one lane that fires at **many** positions. The lane's
`spec` / `role` / `emitter` / `repeat` are shared defaults, and each hit appends its tag(s) to the base
`spec` (so a tag/pool spec picks an intensity-tagged variant per hit). The array/object `at` is exactly
what distinguishes a ladder from a flat entry (whose `at` is a scalar fraction or
`"enter"`/`"exit"`/`"end"`). Two shapes:

```jsonc
// GROUPED - the `at` object is keyed by the tag to append, value = the clip-fraction positions:
{ "spec": "event:Vocal", "role": "lead", "at": { "low": [0.1, 0.3], "loud": [0.8] } }

// ARRAY - ordered, heterogeneous; each entry is a bare position, [pos, "tag", …], or a per-hit object:
{ "spec": "event:Vocal", "role": "lead",
  "at": [ 0.2, [0.5, "loud"], { "at": 0.9, "tags": ["loud"] } ] }
```

The lane's `at` carries the positions; inside a per-hit object the inner `at` is that one hit's position,
a per-hit `spec` replaces the base, and a per-hit `emitter` can override the lane's shared emitter.

A ladder can carry its positions under **`atFrame`** instead (same two shapes), in which case every
bare position is a clip frame. A per-hit object still picks its own key, so a single ladder can mix
the two:

```jsonc
{ "spec": "event:Vocal", "role": "lead",
  "atFrame": [ 6, [15, "loud"], { "at": 0.9, "tags": ["loud"] } ] }
```

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
attributed to the actor selected by `role`. There is no separate lane: audio and the box are
the same clip. The text lives **with the clip in its `*.sounds.json` pool**, so authoring it once gives
that clip a subtitle everywhere it's used.

In a pool, the `clips` value may be the usual **array**, or — the shorthand for subtitled lines — an
**object mapping each clip path to its spoken text**:

```jsonc
// Data/OSF/seduce.sounds.json
{
  "schema": 2,
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

Camera postures are **held**: ledger-tracked and auto-restored to the player's prior POV after any
scene termination. Supported states: `freefly` (pure engine-native free camera using the `tfc` driver),
`scene_orbit` (the default; enters native TFC to retain its close-actor rendering behavior, then uses
the OSF-driven transform to frame and center the
participants — while the scene browser is open, hold **LMB and drag** to orbit so free mouse movement keeps
driving the UI cursor; on a controller use right stick to orbit, left stick to pan, LT/RT to move
vertically, and LB/RB to zoom), `thirdperson_hold` (force and hold third person, bouncing the player back if
they zoom to first person), and `vanity_orbit`.

During a player scene, **MMB** or controller **R3** toggles the engine-native free camera (the same
camera path as `tfc`); pressing it again returns to the scene's prior orbit/vanity/held posture.

A file with no `"camera"` key defaults to **`scene_orbit`** on each scene's entry node. It first enters
the engine's TFC path so the native close-camera actor rendering policy remains active, then switches
transform control to OSF: it centers on the participants' midpoint, pulls back until everyone fits,
and opens side-on to the participants' long axis. Authors can explicitly select `"camera": "freefly"` when they want the
engine's native movement driver instead. Use
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

Set on a node or as a file default. Only the canonical fields are accepted. Per-start caller
overrides persist across transitions; inherited defaults resolve from each destination node.

### Player input lock (`playerInputLock`, default-on)

When the **player is a participant**, the scene engages the player input lock automatically at start,
suppressing normal movement/combat input while leaving camera look available. You do **not** need to
author `osf.control.lock`. The lock is ledger-tracked, so it auto-releases on every termination path.

- Set **`"playerInputLock": false`** to leave the player free (e.g. a scene they only spectate).
- The default never engages for an **NPC-only** scene (no player participant).
- Authored `osf.control.lock` / `osf.control.release` still work: the lock is idempotent (re-locking is
  a no-op), and an authored release can drop it mid-scene.

### Hide apparel (`hideApparel`, default-on)

At scene start the runtime hides **every participant's** worn apparel (the base skin/body is always
kept, so an actor is never made invisible) — you do **not** need to author `osf.equipment.hide`. It is
owned by the scene scope and reconciled on termination. Active equipment/preserve claims
are kept; restoration respects current inventory and newer equipment.

- Set **`"hideApparel": false`** to keep actors clothed (then author per-role `osf.equipment.hide` for
  selective apparel hiding).
- Unlike the player input lock, this applies to **all** participants, including NPC-only scenes.
- Authored `osf.equipment.hide` / `osf.equipment.restore` still work.

**User scene gear** (`docs/RFC-scene-gear.md`) composes with apparel hiding: items registered as scene gear
(via `Data/OSF/**/*.osfgear.json` or the user's `scene-gear.json`) that a participant carries are
auto-equipped for the scene's duration — a worn one is exempted from default apparel hiding instead. A role's
authored `equip` wins over user gear for the slot it occupies; gear fills the slots the scene didn't
touch. Players can disable this globally (settings → Scene gear → Auto-equip scene gear).

### Held-item clear (`clearHeldItems`, default-on)

At scene start the runtime temporarily unequips each participant's equipped **non-apparel** items —
weapons, slates, tools, consumables, and similar held props — so they cannot remain welded to a hand
through an unrelated animation. Optional hide claims track these items and reconcile them on
termination, preserving other owners' equipment claims and newer live inventory changes.
This policy is independent of `hideApparel`: keeping apparel does not
implicitly keep held items.

- Set **`"clearHeldItems": false`** for a partial-body or equipment gesture that deliberately needs to
  preserve the actor's current equipped item.
- Opting out is safest when the scene or its caller already rejects incompatible states such as a drawn
  or aimed weapon; OSF will leave those items completely untouched for that scene.

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
  scene's actor teleport/apparel-hide/camera-cut run synchronously *this* frame, so the initial snap is already
  on screen before black arrives. The default gives a cinematic dip + settle, not a hidden start.
- Authored `osf.fade.out` / `osf.fade.in` still work (e.g. a held end-fade), independent of this default.

### World placement (`placement`)

`placement` governs each participant's **world/root placement** and accepts exactly
`"anchorAndPin"` (the default) or `"followActor"`. Legacy `inPlace: false` maps to
`"anchorAndPin"` maps to `"followActor"`. World placement is independent of
role `poseMode`, which only decides how sampled local bone transforms combine with the live engine
pose. Setting `poseMode: "additive"` does not enable root motion or change `placement`.

By default the runtime uses **anchor-and-pin placement**: participants are teleported to the world
anchor (participant[0]'s transform, or the furniture world anchor) and their rendered root position
**and heading are re-pinned every frame** so multi-actor placements stay aligned. That pin overwrites
the player's heading each frame —
and in vanilla third person the *camera* writes the player's heading, so the two fight and the camera
judders.

Set **`"placement": "followActor"`** for scenes that should play on each actor **exactly where they
stand**:

- No teleport, no per-frame root/heading pin, no animation-driven AI flag — the rig follows the actor's
  live transform.
- The player keeps vanilla camera behavior entirely (pair it with `"camera": "none"` and
  `"playerInputLock": false` for the full hands-off posture — this is the emote-pack recipe).
- Meant for **solo flourishes** (emotes, gestures). A multi-actor follow-actor scene gets no relative
  alignment — each actor animates wherever they happen to be.
- Incompatible with an `anchor` requirement (load error): a world anchor's whole job is positioning
  the participants.

### Scene controls (`sceneControls`)

`sceneControls` configures the scene-control grant; it does **not** control the normal player input
lock described by `playerInputLock`. Scene controls are **enabled by default**: with no
`sceneControls` block the player gets every OSF capability while participating. A scene opts out
wholesale (`"sceneControls": false`) or narrows it:
`{ "disable": ["speed", "end"], "locked": true }`. Capabilities
are advance / navigate / speed / align / freecam / end; `locked: true` means the player may not end
the scene via the scene-control channel (story scenes).

The object form also accepts **`"enabled": <bool>`**, an explicit on/off toggle with the same effect
as the boolean `"sceneControls": true|false` form.

`sceneControls` may also sit at **content-file level**, where it seeds every scene in that file —
`"sceneControls": false` at the document root revokes scene controls for all scenes in that one file,
and the object form narrows the file-level grant. A scene's own `sceneControls` applies **on top of**
what it inherited: a boolean re-enables/revokes wholesale, and a scene `disable` list composes with
the content file's list (both sets of capabilities come off). The file's `pack` label does not extend
this inheritance to other files in the same content pack.

### Furniture world anchoring (`anchor`)

A **world-anchor-bound** scene declares WHAT it plays on with an inline `anchor` block — on the scene, or
at the file level of a multi-scene envelope (a file-level `anchor` is the default every scene in the
file inherits unless it declares its own). Works for linear AND graph scenes; 475 shipped scenes
(the vanilla furniture packs) use it.

```jsonc
{
  "id": "mypack.scenes.barstool",
  "anchor": {
    "keyword": "Starfield.esm|0x0EF961",            // any-of keyword matchers (KYWD form refs; this is one the shipped furniture packs use)
    "base":    ["MyMod.esm|0x800", "MyMod.esm|0x801"], // and/or any-of base-form matchers
    "offset":  { "x": 0, "y": -12, "z": 0, "heading": 180 }  // corrects the ref's transform
  },
  "stages": [ ... ]
}
```

- **`keyword`** — a `"Plugin.esm|0xLocalID"` form ref (or an array of them) naming keyword(s) the
  anchor ref must carry (any-of). The vanilla furniture packs key on the game's `AnimFurn*` keywords.
- **`base`** — a form ref (or array) the anchor ref's base object must match (any-of).
- At least **one of `keyword`/`base` is required** (an empty block could never be satisfied).
- **`offset`** — optional placement correction relative to the resolved anchor: `x`/`y`/`z` local
  units + `heading` degrees.
- Refs resolve **at load**; a malformed/unresolvable ref **rejects the scene** (a file-level one
  rejects the file) with a load error.
- At start, an anchor-bound scene **requires** a satisfying ref: `SceneOptions.Anchor` (Papyrus),
  the browser's furniture pick, or anchor-first matchmaking (`StartSceneAtAnchor`). Starting one
  with no/incompatible anchor fails with handle 0.
- Incompatible with `placement: "followActor"` (or legacy `inPlace: true`; load error — see above).

### Matchmaking (`tags`, `priority`, `weight`)

- **`tags[]`**: free-form matchmaking tags (matched by `StartSceneByTags*`).
- **`priority`** (int, default `0`): matchmaking tier — higher wins.
- **`weight`** (int, default `1`): weighted-random sampling within the top priority tier.

All three may be set at **file level**. `priority` / `weight` are plain defaults a scene overrides.
`tags` are the exception to file-level-is-a-default: they are **UNION-ed** with each scene's own tags
(content-file tags first, de-duplicated case-insensitively), so a tag every scene in that content file
must carry is authored once at the document root rather than repeated per scene. The `pack` label does
not propagate tags to other files in the content pack.
- **`unlisted`** (bool, default `false`): hold the scene **out of the matchmaking pool**  .

Matchmaking is over the content registry's scene namespace: a single query (tags + per-role
gender/keyword/race fit)
returns the best candidate, ranked purely by `priority` tier then `weight`. A richer scene simply sets
a higher `priority` than a barer one. An `unlisted` scene sits outside this entirely.

---

## `osf.*` action vocabulary (built-in mechanisms)

All are **recorded in the per-handle undo ledger and auto-reversed on any scene termination** — you do
**not** need to author the restore half.

`role` is **optional on every action**: an omitted/empty `role` targets the scene's **first
participant** (the same default as the `sound` lane). A named `role` must be declared in `roles[]`
(an undeclared name is a load error). The ✓ below marks which actions act on a role's actor at all.

| Action `type` | Effect | Targets a role | Extra fields |
|---------------|--------|:---:|---|
| `osf.control.lock` / `osf.control.release` | Player input lock (ref-counted). **On by default when the player participates** — see *Player input lock*; author these only to override. | ✓ | |
| `osf.equipment.hide` / `osf.equipment.restore` | Hide / restore the role's worn apparel (skin kept). **All participants' apparel is hidden by default** — see *Hide apparel*; author these only to override. | ✓ | |
| `osf.equipment.equip` / `osf.equipment.unequip` | Equip an arbitrary item on the role for the scene, then take it back off. A missing copy is added and removed on cleanup only while its provenance remains known. Pre-owned items remain in inventory; other owners' claims survive. `unequip` releases authored claims only. See [actor-state ownership](ACTOR_STATE.md). | ✓ | `item` (required on `equip`: form ref `"<Plugin>\|0xLOCAL"`) |
| `osf.weapon.sheathe` / `osf.weapon.restore` | Holster / re-draw the role's weapon. | ✓ | |
| `osf.prop.attach` / `osf.prop.destroy` | Clone a render-only visual, attach it to an actor attachment node under a scene-local id, or destroy that named visual. Re-attaching the same id moves it to the new role/attachment node without cloning it again. | ✓ | `prop`; attach also requires `source` and `attachmentNode` (legacy `node`) — inline or inherited from the file-level `props` registry (`use` names a template) — with optional `position`, `rotation`, `scale` |
| `osf.fade.out` / `osf.fade.in` | Fade screen to/from black. | | `hold` (stay faded on cleanup), `duration` (ramp secs, 0 = default) |
| `osf.voice.play` | Play a sound spec on the role's voice channel. If the clip carries subtitle text, the role actor is its speaker (see *Voice lines*). | ✓ | `set` (required: Data-relative path or `"event:<name>"`); `emitter` (`"listener"` default or `"role"`) |

> **Cleanup is automatic.** The ledger reverses control/camera/weapon/equipment/equipped-items/props/fade in
> reverse order after every termination cause (natural completion, `StopScene` cancellation, runtime
> interruption, or world-load teardown) — you never
> author a restore.

### Scene props

A prop belongs to one live scene handle. Its `prop` value is an author-local id within that scene,
not a persistent object identity. OSF destroys every remaining prop before it dispatches the scene-end
event and also detaches them explicitly during world-load teardown. A consumer that wants a visual to
remain after the animation should create and own a separate persistent object.

```jsonc
"action": [
  {
    "at": 0.101,
    "type": "osf.prop.attach",
    "role": "player",
    "prop": "helmet",
    "source": {
      "equippedArmor": {
        "keyword": ["ArmorTypeSpacesuitHelmet", "ArmorTypeHelmet"]
      }
    },
    "attachmentNode": "R_Wrist",
    "position": [0, 0, 0],
    "rotation": [0, -90, -90],
    "scale": 1
  },
  { "at": 0.758, "type": "osf.prop.destroy", "prop": "helmet" }
]
```

`source` contains exactly one selector:

- `{ "form": "<Plugin>|0xLOCAL" }` clones a fixed bound object's ground visual.
- `{ "equippedArmor": { "keyword": "<EditorID>" } }` selects the first equipped armor carrying
  that keyword. `keyword` may be an array of alternative editor IDs.

`attachmentNode` is the preferred field remains accepted. Supplying both on one action
or prop template is a load error.

`position` and `rotation` are three-number arrays in attachment-node-local units and degrees; both default to
zero. `scale` defaults to `1` and must be in `(0,10]`. All three are optional — omitting them is
exactly equivalent to authoring `[0,0,0]` / `[0,0,0]` / `1`, so an identity attachment can drop them.
The visual is best-effort: a missing source,
actor 3D, or attachment node logs the failed action without terminating the scene.

#### Prop registry (file-level `props`)

Repeating a `source` + `attachmentNode` block on every attach is the common case, so a file may declare its
props **once** at the top level and let each action name one. The registry is a file-local map of an
**exact, case-sensitive id** to a prop definition:

```jsonc
{
  "schema": 2,
  "props": {
    "helmet": {
      "source": { "equippedArmor": { "keyword": ["ArmorTypeSpacesuitHelmet", "ArmorTypeHelmet"] } },
      "attachmentNode": "R_AnimObject1"
    }
  },
  "nodes": [ /* ... */
    "action": [
      { "at": 0.058, "type": "osf.prop.attach", "role": "player", "prop": "helmet" },
      { "at": 0.798, "type": "osf.prop.destroy", "prop": "helmet" }
    ]
  ]
}
```

**An attach's `prop` id is itself the lookup key** — the common case needs no reference syntax at
all. `use: "<id>"` overrides which template to pull, for a second instance of one definition:

```jsonc
{ "at": 0, "type": "osf.prop.attach", "prop": "helmet_l", "use": "helmet", "attachmentNode": "L_AnimObject1" }
```

`prop` stays the runtime id either way — it is what `osf.prop.destroy` addresses and what a
re-attach matches. Two ids sharing one template are two independent clones.

- **Inline keys win.** Any of `source`, `attachmentNode`, `position`, `rotation`,
  `scale` authored on the action overrides the template; the rest are inherited. An inline attachment
  node overrides the template attachment node.
- **`source` is inherited or overridden whole**, never merged key-by-key — so an action may select
  `{ "form": … }` over an `equippedArmor` template without the two selectors colliding.
- **Templates may be partial.** A definition carrying only `source` lets each action pick its own
  `attachmentNode`. Only the *merged* result must be a complete attachment.
- A template holds **only** those five concepts: `source`, `attachmentNode`,
  `position`, `rotation`, and `scale`. Timing (`at`/`atFrame`/`repeat`), `type`, `role` and
  `prop` are never inheritable, so a template can neither move an action in time nor retarget it;
  any other key is a load error.
- A `prop` id matching no template is an ordinary self-contained attach — **files without `props`
  are entirely unaffected.**

Prop templates remain file-local. Route edges can cross files; prop templates cannot.

---

## Validation (at load)

Diagnostics include source file, global node ID, role and clip where known. Invalid nodes are skipped;
a malformed shared role/prop template rejects its entire file. Duplicate global IDs keep the first
loaded definition. A node must have exactly one `clip`/`stages` source, a valid cast and consistent
clip counts. Missing/rejected edge destinations reject dependent routes. Retired container, splice,
hold-grant and alias keys report migration errors. Prop action `use` must name a file-local template;
node `use` is not supported. No synthetic ID namespace is reserved.

---

## Well-known tags (the immersion tag contract)

These tags are **matched by name** by the browser and hotkey/wheel features, so third-party packs
can join them without another schema layer (the shipped set lives in `Data/OSF/immersion/`):

| Tag | Consumed by | Contract |
|-----|-------------|----------|
| `player.emote.<name>` | Browse as an Emote and the default Quick Access wheel | Solo, free-space, and **self-terminating** (`timer`/`loops`, not an unbounded hold); the emote's `name` is the wheel slice label. Older UI may call this category `Action`, which is unrelated to the authored `action` track. The same launch preset runs on a crosshair NPC target, so keep the role anonymous/unfiltered unless the clip demands otherwise. |
| `immersion` | — | Umbrella tag for the shipped immersion pack; free for browsing/filtering. |
