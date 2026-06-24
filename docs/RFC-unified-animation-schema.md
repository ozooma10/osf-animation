# RFC: One scene concept (collapse packs & scenes)

> **Status:** IMPLEMENTED (Phases 1–5 landed) · **Scope:** pre-launch, no backwards-compatibility
> **Audience:** the implementing agent. Self-contained — you do not need the conversation that produced
> it. Read [RFC.md](RFC.md) §3–4 for framework philosophy and [SCENE_SCHEMA.md](SCENE_SCHEMA.md) for the
> author-facing reference.
>
> **Two pragmatic deviations from the plan as written:**
> 1. **`Slot::Kind` deletion + `Start*` merge** were folded into Phase 5 (not Phase 2) — they were
>    load-bearing for legacy packs/files until those were removed. `Slot::Kind` is now gone; the
>    `StartFrom*` method names were kept (cosmetic merge skipped — they already converge on `MintSlot`).
> 2. **`ReloadPacks` was NOT renamed to `Reload`.** Renaming the native requires regenerating
>    `OSF.pex` (Papyrus toolchain), which is out of band. The native keeps its name + signature and
>    now reloads the single registry. Rename when scripts are next recompiled.

## 0. TL;DR for the implementing agent

Today OSF has **two content concepts**: *animation packs* (`*.json`, a linear `stages[]`→`clips[]`
timeline) and *scenes* (`*.scene.json`, a node graph whose nodes reference a pack animation by id via
`anim`). This RFC **collapses them into one concept: a scene.** A scene is *minimal by default* (just
clips) and *expands into graph features* (nodes, edges, tracks, roles, policy) only when needed. There
is no separate "pack" or "animation" entity anymore — **everything an author writes is a scene**, and a
running instance is also a scene.

The decisive fact that makes this cheap: **the playback engine is already unified.** Both kinds lower
into the same `Animation::ScenePlan` → `Animation::Scene` (a flat stage player) via the same
`BuildScenePlan` + `GraphManager::PlaySceneStaged`. The only thing distinguishing them at runtime is a
one-byte `SceneRuntime::Slot::Kind` tag and ~3 branch sites. So this is a **schema + registry + loader**
change, plus deleting that tag. **`src/Animation/*` (the engine) does not change.**

Because OSF has **not launched**, there is no ABI to preserve and no migration window. The plan:
**extend `SceneDef`/`SceneRegistry` to absorb packs, delete `PackRegistry`/`AnimationDef` entirely,
rename freely, and rewrite the fixtures.**

---

## 1. The model

Everything is a **scene** (decision §11.1). Use these words precisely in code and docs:

| Term | Meaning |
|------|---------|
| **scene** (definition) | the unified content entity an author writes (`SceneDef`). Minimal = clips; can grow a graph. Replaces both "pack animation" and the old "scene". |
| **scene** (instance) | a *running* scene: a handle + anchor + participants + undo ledger. Unchanged runtime concept. `StartScene` starts an instance from a definition. |
| **node** | one step inside a scene's graph (`SceneNode`). Plays inline `stages` **or** `use`s another scene. |

There is no longer an "animation" noun for content — a clip is a clip (a `.glb`/`.af` file), and the
thing that sequences clips is a scene. (`Animation::Scene` / `GetCurrentAnimation` etc. keep their
names; those are the engine playback layer and the clip path, not the content entity.)

### File format
- **One file kind:** `Data/OSF/**/*.osf.json` (decision §11.2).
- A file is `{ "schema": 1, "name?": "...", "scenes": [ <scene>, ... ] }`, **or** a single bare
  `<scene>` object for the one-scene-per-file case.
- File-level `lockPlayer` / `stripActors` are optional defaults each scene may override (keeps the
  current pack file-default → per-entry override convenience).
- `//` line comments stay allowed (nlohmann parse flag, as today).

### A scene is linear OR a graph — by shape, not by type
- **Linear** (the common case): top-level `clip` or `stages[]`, no `nodes[]`. Desugars at load into a
  single auto-advancing node chain (§4).
- **Graph**: top-level `nodes[]` (+ `entry`). Presence of `nodes[]` is the discriminator.
- A node plays **exactly one of**: inline `stages[]` (the default; a one-off), or `use: "<sceneId>"`
  (reuse another scene by id — the old `anim` indirection, demoted to opt-in). Both, or neither, is a
  hard load error.

---

## 2. The unified schema (authoring)

```jsonc
// ---------- MINIMAL: a one-shot solo (plays once, then ends) ----------
{ "schema": 1, "id": "author.wave", "clip": "OSF/Anims/Wave.glb" }
// `clip` is sugar for: roles:[{}], stages:[{ clips:["OSF/Anims/Wave.glb"] }]

// ---------- MINIMAL held idle (loops forever until stopped) ----------
{ "schema": 1, "id": "author.idle",
  "stages": [ { "loops": 0, "clips": ["OSF/Anims/Idle.glb"] } ] }
// loops:0 / timer:0 => hold. (A stage with NEITHER set plays once then advances — current rule.)

// ---------- MULTI-STAGE LINEAR (== today's solo.playthrough) ----------
{ "schema": 1, "id": "author.playthrough",
  "tags": ["solo"], "lockPlayer": true, "stripActors": true,
  "roles": [ {}, { "offset": { "y": 1.0, "heading": 180.0 } } ],   // optional; else inferred from clips
  "stages": [
    { "clips": ["A.glb", "A.glb"] },                                // play once
    { "timer": 6.0, "clips": ["B.glb", { "file": "B2.glb", "offset": { "y": 1.5 } }] },
    { "loops": 0,   "clips": ["C.glb", "C.glb"] }                   // hold; last stage ends the scene
  ] }

// ---------- ADVANCED GRAPH: branch + self-loop + loop-count + tracks + roles + reuse ----------
{ "schema": 1,
  "id": "author.scenes.demo", "name": "Demo Scene",
  "priority": 5, "weight": 2,                          // matchmaking
  "tags": ["paired", "demo"],
  "lockPlayer": true, "stripActors": true,
  "playerControl": { "disable": ["speed"], "locked": true },
  "roles": [                                           // named roles + form filters
    { "name": "lead",  "gender": "any" },
    { "name": "other", "gender": "female", "keywords": ["Starfield.esm|0x...", "..."] }
  ],
  "entry": "approach",
  "nodes": [                                           // presence of nodes[] => graph scene
    { "id": "approach",
      "use": "author.shared.walkin",                   // REUSE: by-id ref into the one namespace
      "loop": { "mode": "once" },
      "edges": [ { "to": "main", "when": "end" } ] },

    { "id": "main",
      "stages": [ { "loops": 0, "clips": ["Main.glb", "Main2.glb"] } ],   // INLINE (default for one-offs)
      "loop": { "mode": "hold" },
      "cue":    [ { "at": 0.5,    "id": "beat" } ],                       // `at` = named anchor OR [0,1) fraction
      "action": [ { "at": "enter", "type": "osf.fade.in" } ],
      "sound":  [ { "at": 0.5,    "spec": "event:Music", "role": "lead" } ],
      "camera": [ { "at": "enter", "state": "thirdperson_hold" } ],
      "edges": [
        { "id": "finish", "label": "Finish", "to": "climax", "when": "advance", "default": true },
        { "id": "tease",  "label": "Tease",  "to": "main",   "when": "advance" }   // self-loop
      ] },

    { "id": "climax", "use": "author.shared.peak", "loop": { "mode": "count", "count": 3 },
      "edges": [ { "to": "cooldown", "when": "loops" } ] },

    { "id": "cooldown", "use": "author.shared.winddown", "loop": { "mode": "once" } }
  ] }
```

### Field reference (authoring → internal)
- **`clip`** (string): sugar → `stages:[{ clips:[clip] }]` with one inferred role.
- **`stages[]`**: `{ timer?, loops?, clips[] }`. `clips[i]` is a bare Data-relative path or
  `{ file, offset? }`. Bare-array stage shorthand (`["a.glb","b.glb"]`) still allowed. Same semantics as
  today's `StageDef` / `StageClip` — keep them.
- **`roles[]`** (optional): the single participant model (decision §11.4) — unifies today's pack
  `actors` (`SlotDef`) and scene `roles` (`SceneRole`). Each:
  `{ name?, gender?, keywords?[], races?[], offset? }`. `name` omitted → anonymous positional slot.
  `keywords`/`races` are form-ref strings resolved at load (any-of within each; AND across present
  constraints). `offset` is the role's default placement. Clips in `stages[].clips` index-align to
  `roles` order. Omit `roles` → count inferred from the first stage's clips, all gender `any`.
- **policy:** `lockPlayer` (bool, default true), `stripActors` (bool, default true),
  `playerControl` (the `PlayerControl` block; default enabled). Same semantics as today.
- **matchmaking:** `tags[]`, `priority` (int, default 0), `weight` (int, default 1).
- **node:** `{ id, (use | stages), loop?, timerSec?, edges?[], cue?[], action?[], sound?[], camera?[] }`.
  `loop` = `{ mode: "once"|"hold"|"count", count? }`. Keep the existing `SceneNode` loop/edge/track
  fields; add the inline-`stages` alternative to `use`.
- **edges:** unchanged `SceneEdge` (`id,label,labelKey,to,when,trigger,default,priority`). `to` is a
  node id or `"$end"`. `when` ∈ `end|loops|timer|advance|trigger`.
- **tracks:** keep `CueEntry`/`ActionEntry`/`SoundEntry`/`CameraEntry`. **Timing uses a single `at`
  field** (decision §11.3): a named anchor `"enter"|"exit"|"end"` **or** a numeric clip-fraction in
  `[0,1)`, plus optional `repeat:"loop"` (numeric only). The parser lowers `at` → the internal
  `(pos, fraction, everyLoop)`. This matches what [SCENE_SCHEMA.md](SCENE_SCHEMA.md) documents.

---

## 3. Internal data model

**Extend `SceneDef`/`SceneRegistry` in [SceneRegistry.h](../src/Registry/SceneRegistry.h)** to absorb
packs; **delete `PackRegistry`/`AnimationDef`/`SlotDef`/`PackPolicy`** ([PackRegistry.h](../src/Registry/PackRegistry.h)).
Keep the leaf structs `StageClip`, `StageDef`, `SlotGender`, `ParseSlotGender` — **move them into
SceneRegistry.h** (they currently live in PackRegistry.h, which goes away).

```cpp
// src/Registry/SceneRegistry.h  (extended; PackRegistry.h deleted)
namespace OSF::Registry {

  // KEEP (move StageClip/StageDef/SlotGender/ParseSlotGender here from the deleted PackRegistry.h):
  //   SlotGender, ParseSlotGender, StageClip, StageDef,
  //   LoopMode, EdgeWhen, SceneEdge, CuePos/ActionPos/SoundPos/CameraPos,
  //   CueEntry, ActionEntry, SoundEntry, CameraEntry, PlayerControl.

  struct Role {                       // was SceneRole; now also covers former pack `actors`
    std::string                  name;        // "" = anonymous positional slot
    SlotGender                   gender = SlotGender::kAny;
    std::vector<RE::BGSKeyword*> keywords;    // resolved at load; empty = no constraint
    std::vector<RE::TESRace*>    races;        // resolved at load; empty = no constraint
    Animation::ParticipantPlacement offset{}; // default placement (was SlotDef::offset)
  };

  struct SceneNode {                  // gains inline stages; `anim` -> `use`
    std::string              id;
    std::string              use;            // referenced scene id ("" if inline)
    std::vector<StageDef>    stages;          // inline timeline (empty if `use`)
    LoopMode                 loopMode = LoopMode::kHold;
    std::int32_t             loopCount = 0;
    float                    timerSec = 0.0f;
    bool                     loopForever = false;
    std::vector<SceneEdge>   edges;
    std::vector<CueEntry>    cues;
    std::vector<ActionEntry> actions;
    std::vector<SoundEntry>  sounds;
    std::vector<CameraEntry> cameras;
  };

  struct SceneDef {                   // the ONE entity (absorbs AnimationDef)
    std::string              id;
    std::string              name;
    std::filesystem::path    sourceFile;
    std::vector<std::string> tags;
    std::int32_t             priority = 0;
    std::int32_t             weight = 1;
    bool                     lockPlayer = true;
    bool                     stripActors = true;
    PlayerControl            playerControl;
    std::vector<Role>        roles;          // was std::vector<SceneRole>
    std::string              entry;          // entry node id (filled by desugar for linear)
    std::vector<SceneNode>   nodes;          // always non-empty post-load (desugar fills it)
    std::vector<std::string> linearStages;   // stage i -> node id (filled by desugar for linear)

    const SceneNode* FindNode(std::string_view) const;
    std::int32_t     LinearStageOf(std::string_view) const;
  };

  class SceneRegistry {              // now the ONLY content registry
  public:
    static SceneRegistry& GetSingleton();
    void LoadAll();                                   // scans Data/OSF/**/*.osf.json
    const SceneDef* Find(std::string_view) const;     // case-insensitive
    // Resolve a node's playable (inline stages OR a `use` target) to a ScenePlan.
    std::optional<Animation::ScenePlan> BuildNodePlan(const SceneDef&, const SceneNode&, size_t actorCount) const;
    void ForEachDef(const std::function<void(const SceneDef&)>&) const;  // matchmaker
    size_t Size() const;
    std::vector<std::string> LoadErrors() const;
  private:
    mutable std::shared_mutex lock;
    std::unordered_map<std::string, SceneDef> scenes;  // ONE map, key = lowercased id
    std::vector<std::string> loadErrors;
  };
}
```

`BuildNodePlan` is the merge point of today's two paths: for an inline node it builds the plan from
`node.stages` directly (the body of `PackRegistry::BuildScenePlan`,
[PackRegistry.cpp:316-347](../src/Registry/PackRegistry.cpp)); for a `use` node it looks up the target
`SceneDef`, takes its **entry node's stages** (only single-/linear-resolvable targets — see §9), and
builds from those.

---

## 4. The desugar (the key trick)

At load, after parsing, **every linear scene is rewritten into a node chain** so the runtime only ever
sees graph-shaped data. This is what lets `Slot::Kind` and its branches be deleted.

```
desugar(SceneDef d):
  if d.nodes is non-empty: return   # already a graph; entry is author-set
  # linear: build one node per stage, linked by the stage's own advance condition
  for i, stage in enumerate(d.stages):
    node.id = "#s{i}"                # RESERVED synthetic id; '#' is illegal in authored ids
    node.stages = [stage]
    when = stage.timer>0 ? "timer" : stage.loops>0 ? "loops" : "end"
    target = (i == last) ? "$end" : "#s{i+1}"
    node.edges = [ { to: target, when: when } ]
    d.nodes.append(node)
  d.entry = "#s0"
  d.linearStages = ["#s0", ... "#s{n-1}"]   # so GetSceneStage/SetSceneStage work
```

The terminal node's edge to `$end` with the matching `when` reproduces today's `OnGraphAutoEnd`
"final stage ends the scene" behavior **without** the `kind != kDef` check.

---

## 5. Runtime changes (`src/Scene/SceneRuntime*`)

The engine (`src/Animation/*`) is untouched. Changes are localized.

> **Sequencing note (decided during implementation):** item 3 (`PlayNodeAnim` retarget) is all of
> Phase 2 — it's the only change needed to make unified scenes *play*, because Phase 1's desugar already
> routes every linear `.osf.json` scene through the existing `kDef` path (and graph scenes are `kDef` by
> construction). Items 1–2 (deleting `Slot::Kind`, merging `Start*`) are **deferred to Phase 5**: the
> `kPack`/`kFiles` tags remain load-bearing for legacy packs and ad-hoc files — e.g. `GetStage`/`SetStage`
> delegate to the GraphManager's in-`Scene` stage jump for `kPack` vs `linearStages` for `kDef` — so they
> can only be removed once `PackRegistry`/`StartFromFiles` are.

1. **Delete `Slot::Kind`** (enum + `Slot::kind` + `SlotView::kind`) — *(Phase 5; see note above)* —
   [SceneRuntime.h:143-148, 167, 189](../src/Scene/SceneRuntime.h). Collapse its branch sites to the
   (former) `kDef` path:
   - `OnGraphAutoEnd` `kind != kDef` terminal-vs-edge decision —
     [SceneRuntime_Graph.cpp:700-706](../src/Scene/SceneRuntime_Graph.cpp). After desugar, the auto-edge
     walk handles termination uniformly (terminal node's `$end` edge).
   - `GetStage`/`SetStage` per-kind branch — [SceneRuntime.cpp:223-257](../src/Scene/SceneRuntime.cpp).
     Always use `linearStages` (filled for every linear scene by desugar). A genuine multi-way graph has
     no `linearStages` → returns `-1`/false, same contract.
   - Per-kind default-policy plumbing → always read `lockPlayer`/`stripActors`/`playerControl` off the
     one `SceneDef`.
2. **Merge the `Start*` entry points** into one. `StartFromPack`/`StartFromDef`/`StartFromDefAt`/
   `StartFromDefRoles` ([SceneRuntime.h:79-107](../src/Scene/SceneRuntime.h)) already converge on
   `MintSlot` + `PlayNodeAnim`/`PlaySceneStaged`. Collapse to:
   - `Start(sceneId, participants, anchor?)` — look up the `SceneDef`, enter at `def.entry`.
   - `StartRoles(sceneId, actors, roles)` — same, with named-role binding (now valid for any scene that
     declares `roles`).
   - `StartFiles(...)` — keep for ad-hoc clip lists; it already builds a synthetic single-node scene.
     Drop the `kFiles` tag; it becomes "an unregistered linear scene."
   - `MintSlot` loses its `Kind` parameter.
3. **`PlayNodeAnim`** — [SceneRuntime_Graph.cpp:247-284](../src/Scene/SceneRuntime_Graph.cpp): resolve
   via `SceneRegistry::BuildNodePlan(def, node, n)` instead of `PackRegistry::BuildScenePlan(node->anim, n)`.
   The `ApplyNodePolicy`/`ApplyNodeMarks` stamping (incl. the "multi-stage used as a node" reconciliation
   at [SceneRuntime_Graph.cpp:68-93](../src/Scene/SceneRuntime_Graph.cpp)) stays — it already handles a
   node whose plan has multiple stages.
4. **Dispatch/Ledger slices** (`SceneRuntime_Dispatch.cpp`, `SceneRuntime_Ledger.cpp`) already early-out
   when a node has no track entries; a desugared linear node simply has empty lanes. No change.

---

## 6. Matchmaker changes (`src/Matchmaking/Matchmaker.*`)

- `BuildPool` ([Matchmaker.cpp:124-184](../src/Matchmaking/Matchmaker.cpp)) iterates **one**
  `SceneRegistry::ForEachDef` instead of `SceneRegistry::ForEachDef` + `PackRegistry::ForEachAnim`.
- **Delete** `Candidate::Source` ([Matchmaker.h:31-36](../src/Matchmaking/Matchmaker.h)) and the same-id
  **shadow rule** ([Matchmaker.cpp:131, 161-163](../src/Matchmaking/Matchmaker.cpp)) — one namespace has
  no collision to shadow.
- **Delete** the pack pseudo-candidate `priority=0/weight=1` special-case
  ([Matchmaker.cpp:170-171](../src/Matchmaking/Matchmaker.cpp)); every `SceneDef` carries real
  `priority`/`weight` (defaults 0/1 preserve today's ranking for un-prioritized scenes).
- `Pick`/weighted-random tiering otherwise unchanged.

> Update RFC.md §2.6 ("packs are the baseline, scenes win when they exist"): the baseline-vs-authored
> relationship is now expressed purely through `priority` — a richer scene sets a higher `priority` than
> a bare one. The pack-vs-scene framing no longer applies.

---

## 7. Papyrus / native API (`src/Papyrus/OSFScript.*`, `dist/Scripts/Source/OSF.psc`)

Pre-launch, so finalize names now. The scene-centric verbs already fit (everything is a scene):

- **`StartScene(actors, sceneId, opts)`** — [OSFScript.cpp:302-325](../src/Papyrus/OSFScript.cpp): one
  registry lookup. **Delete `SplitScenePrefix`** ([OSFScript.cpp:114-126](../src/Papyrus/OSFScript.cpp))
  and the scene-first/pack-fallback branch — one namespace, one lookup. `opts.Stage` applies to any
  linear scene.
- **`StartSceneByTags` / `StartSceneByTagsQuery`** — unchanged signatures; internal `Candidate.source`
  branch collapses to one start path.
- **`StartSceneRoles`** — unchanged; now valid for any scene declaring `roles`.
- Handle-/actor-keyed surface unchanged in signature and behavior (the `Slot::Kind` branch just
  disappears): `StopScene`, `StopSceneForActor`, `GetSceneStage`/`ForActor`, `SetSceneStage`/`ForActor`,
  `AdvanceScene`, `NavigateScene`, `GetSceneEdgeCount`/`Id`/`Label`, `Register`/`UnregisterSceneCallback`,
  `IsPlaying`, `GetCurrentAnimation`, `Play`, `Stop`, `SetSpeed`/`GetSpeed`, `SetAnchor`/`ClearAnchor`,
  `IsReady`, `GetVersion`.
- **Rename `ReloadPacks()` → `Reload()`** ([OSFScript.cpp:184-192](../src/Papyrus/OSFScript.cpp)):
  reloads the one registry, returns the unified scene count.

`SceneOptions` / `SceneEvent` structs and the `EVENT_*`/`RESULT_*` constants are unchanged.

---

## 8. Reuse model (`use`) — keep it, demote it

Measured reality in the current corpus: genuine 1:N authored reuse is **one** `solo`↔`solo.cover` toggle
across 3 scenes; the high reference counts are all test scaffolding pointing at one inert pose, and
`demo`'s four `author.pack.*` refs are **dangling** (the targets exist in no file). So:

- **Inline `stages` is the default** authoring path — a node carries its own clips, no cross-file id.
- **`use: "<sceneId>"` stays first-class but optional** for real sharing. Same lazy resolution as
  today's `node->anim`, against the one registry.
- This directly fixes the `demo` drift class once §9 validation lands.

---

## 9. Validation (do this — it's free here)

1. **`use`-resolution at load:** resolve every `use` against `SceneRegistry`. A dangling `use` is a load
   error surfaced via `LoadErrors()` / `GetSceneLoadErrors`. (Today `anim` is unchecked until node enter
   — that's why `demo` ships broken.)
2. **`use` target must be single-/linear-resolvable:** a `use` only splices the target's *entry node's
   stages*. If the target is a multi-way graph, emit a load error.
3. **Node `use` XOR `stages`:** both, or neither, is a hard parse error with a clear message.
4. **Reserved ids:** reject any authored id containing `#` (reserved for synthetic desugar nodes).
5. **Id collisions:** within the one namespace, duplicate id = first-loaded-wins + a logged warning
   (today's within-registry behavior at [PackRegistry.cpp:246-253](../src/Registry/PackRegistry.cpp),
   [SceneRegistry.cpp:625-631](../src/Registry/SceneRegistry.cpp)).

---

## 10. Content & test migration

No back-compat, so rewrite rather than dual-load:

- **`dist/OSF/`:** fold `OSFTestPack.json`'s 9 entries and the 13 `*.scene.json` files into `*.osf.json`.
  Most scenes are single-node and become a bare linear scene. Rewrite `demo` as a graph scene that
  **inlines** its four poses (or ships the missing `author.shared.*` targets) — eliminates the dangling
  refs.
- **`test/fixtures/Data/OSF/`:** rewrite the pack/scene fixtures (`pack_*.json`, `*.scene.json`) into the
  unified format. The dup/shadow/policy/badschema fixtures now test **one** registry's dedup,
  collision-warning, policy resolution, and schema-version reject.
- **Tests:** merge `test/unit/test_pack_registry.cpp` into `test/unit/test_scene_registry.cpp`. Keep
  `test_scene_math.cpp`, `test_frameclock.cpp`, `test_matchmaker.cpp` (matchmaker test updates for the
  single pool). Add desugar tests (§4) and validation tests (§9).
- **Docs:** [SCENE_SCHEMA.md](SCENE_SCHEMA.md) is the unified schema doc (replaced PACK_SCHEMA.md); update [RFC.md](RFC.md)
  §3 (two-concept table → one), §2.6 (matchmaking framing), §4A/§4B (pack vs scene authoring → one
  scene), and the "SCHEMA … TOO VERBOSE" note at §4B. Update [API.md](API.md) for `Reload()` and the
  dropped prefixes.
- **Build:** `xmake.lua` if it references file globs/sources for the deleted `PackRegistry.cpp`.

---

## 11. Decisions (locked)

1. **Entity noun:** one noun — **scene**. The definition and the running instance are both "scene"; no
   "animation"/"pack" content noun. (Disambiguate in prose as "scene definition" vs "running scene".)
2. **File extension:** **`*.osf.json`** (single kind; `*.json`/`*.scene.json` loaders deleted).
3. **Track timing:** single **`at`** field (named anchor or `[0,1)` fraction), lowered to the internal
   `pos`+`fraction`.
4. **Participants:** one **`roles[]`** list (`name` optional); `actors`/`SlotDef` deleted, folded into
   `Role`.

---

## 12. Implementation phases (ordered; each independently testable)

> Engine (`src/Animation/*`) is out of scope throughout — do not touch it.

**Phase 1 — Registry + parser + desugar.** Extend `SceneDef`/`SceneNode`/`SceneRegistry`; move
`StageDef`/`StageClip`/`SlotGender` over; add `Role`. One parser for `*.osf.json` (assemble from the
existing `ParseAnimation` [PackRegistry.cpp:49-171](../src/Registry/PackRegistry.cpp) and `ParseScene`
[SceneRegistry.cpp:435-593](../src/Registry/SceneRegistry.cpp) helpers — most field parses port
directly). Implement the §4 desugar, `at`-lowering, and §9 validation. **Accept:** `test_scene_registry.cpp`
loads minimal/multi-stage/graph fixtures; desugar produces correct node chains + `linearStages`;
dangling `use`, `use`-XOR-`stages`, reserved-id, and collision cases all error/warn as specified.

**Phase 2 — Runtime playback (done).** Add `SceneRegistry::BuildNodePlan` and retarget `PlayNodeAnim`
to resolve unified nodes (inline `stages` / `use`) through it, falling back to `PackRegistry` for legacy
`anim` nodes (§5.3). No `Slot::Kind`/`Start*` changes — those move to Phase 5 (see §5 sequencing note),
since the desugar already routes unified scenes through `StartScene`→`StartFromDef`→`PlayNodeAnim`→
`OnGraphAutoEnd` and through matchmaking unchanged. **Accept:** `BuildNodePlan` unit tests pass; both
targets compile; in-game, a linear scene auto-ends and a graph scene branches/self-loops/loop-counts
(live playback verified in Starfield — out of offline-test scope).

**Phase 3 — Matchmaker.** One pool; delete `Candidate::Source` + shadow + pack pseudo-candidate (§6).
**Accept:** `test_matchmaker.cpp` ranks the single pool by priority-tier then weight; defaults 0/1
reproduce prior picks for un-prioritized scenes.

**Phase 4 — API.** One-lookup `StartScene`, drop prefixes, rename `ReloadPacks`→`Reload` (§7).
**Accept:** `OSF.psc` compiles; `StartScene`/`StartSceneByTags`/`StartSceneRoles`/`Reload` resolve
against the one registry.

**Phase 5 — Content + docs + delete dead code.** Rewrite `dist/OSF/` and fixtures into `*.osf.json`
(§10); fix `demo`; **delete `PackRegistry.{h,cpp}`, `AnimationDef`, `SlotDef`, `PackPolicy`, the
`*.json`/`*.scene.json` loaders**, and **the legacy `SceneNode.anim` field + the legacy `ParseScene`
path**. With packs/files gone, also do the deferred **runtime collapse** (§5 items 1–2): delete
`Slot::Kind` + its branches, merge the `Start*` entry points, and drop the `PlayNodeAnim` legacy-`anim`
fallback. Rewrite the schema doc and update RFC.md/API.md. **Accept:** clean build (`xmake`); full test
suite green; `grep` finds no remaining `PackRegistry` / `AnimationDef` / `Slot::Kind` / `*.scene.json` /
`ReloadPacks` / `SceneNode::anim` references.

---

## 13. Non-goals
- No change to `src/Animation/*` (the playback engine).
- No new playback capability (anchoring, sync, blend, tracks behave as today).
- No save-load/resume work (still out of scope per RFC.md §5).
- Not adding new `osf.*` mechanisms — purely a schema/registry/runtime-shape unification.
