# OSF Scene Design — public contract + internal design

*Design spec, revised 2026-06-17 after two rounds of external API review. **Implementation status:
phases A+B+C are now implemented and in-game tested** (the scene runtime, all four track lanes, the
built-in `osf.*` actions, the generalized undo ledger, settings precedence, and validation — build
slices 1–18). Part 1 describes the surface that **freezes at 1.0**; with A+B+C landed it is now the
freeze candidate — treat Part-1 signatures as stable (the one allowed pre-1.0 break, `StartScene*`
bool/string→int handle, already landed). Part 2 is non-contract and may change freely.*

**How to read this doc.**

- **Part 1 — Public contract.** Papyrus signatures, the `*.scene.json` schema, callbacks,
  lifecycle/cleanup, validation, versioning. What content authors and consumer mods depend on. This
  describes the *end state* that freezes at 1.0 — not a phase boundary (see §2.5 for build order).
- **Part 2 — Internal design.** Architecture, merge rationale, the A/B/C layering, phasing, open
  questions. May change as long as Part 1 holds.

The model in one line: **a scene is a graph of nodes; a node is one animation plus an *optional*
loop-relative track timeline.** Composition (how nodes connect/branch) is the graph; scheduling
(what fires during a node) is the tracks. The common node has **no tracks** — sounds, vocals and
undress come from animation metadata + user settings; tracks are the opt-in power layer.

---

# PART 1 — PUBLIC CONTRACT

## 1.1 Tier 0 — Primitives *(frozen, ships today)*

Bones and clocks, no world side-effects: `Play` · `Stop` · `SetSpeed`/`GetSpeed` ·
`SetAnchor`/`ClearAnchor` · `Sync` · `PlaySequence` · `GetCurrentAnimation` · `IsPlaying`.
Canonical signatures: [API.md](API.md) / `OSF.psc`. Everything below builds on these.

## 1.2 Tier 1 — Runtime API

### Scene-instance handles

A running scene is identified by an **opaque `int` handle** returned from a `Start*` call — never by
an actor.

- `0` is the null handle; a failed start returns `0`.
- **Handles are session-scoped and do not survive save/load.** A world-replacing load force-drops
  all scenes (`GraphManager::StopAll`); after such a load a stashed handle is **dead** and
  `GetSceneForActor` normally returns `0`. Consumers must treat the scene as ended and start a new
  one if appropriate — there is nothing to "re-acquire." (No reload path currently preserves live
  scenes; if one is ever added it will be named explicitly.)
- A handle is invalidated the instant its scene ends. Calls on a dead/invalid handle fail safely
  per the sentinel table below — never assert.
- **Handles (and callback tokens) are generational** — the `int` encodes `[generation | slot]`, so a
  recycled table slot never collides with a stale handle: an old handle whose slot was reused reads
  as invalid, not as the new occupant. `0` is the null handle.

**Integer return sentinels** (no blanket "0 means none" rule — defined per call):

| Call | Sentinel |
|---|---|
| `StartScene*` | `0` = start failed |
| `GetSceneForActor` | `0` = actor not in a scene |
| `GetSceneStage` | `-1` = invalid handle **or** non-linear scene |
| `GetSceneEdgeCount` | `0` = invalid handle **or** no branchable edges |

(The earlier `GetCallbackEvent`/`GetCallbackResult`/`GetCallbackTime` *dispatch-time getters* were
**removed** — the Phase-A prototype proved no synchronous C++→Papyrus path exists, so the event is
snapshotted into the `OSFEvent:SceneEvent` struct argument instead. Read those fields directly:
`akEvent.eventType`, `akEvent.result`, `akEvent.time`/`akEvent.anchor` — see the Callback contract below.)

```papyrus
; --- start (all return an int scene handle; 0 = failed; all actor-first for consistent arg order) ---
int Function StartScene(Actor[] akActors, string asId, int aiStage = 0) Global Native
int Function StartSceneAt(Actor[] akActors, string asId, ObjectReference akAnchor, float afHeadingDeg = -1.0) Global Native  ; world-anchor at a thing, not actor[0]
int Function StartSceneRoles(Actor[] akActors, string asId, string[] asRoles, int aiStage = 0) Global Native
int Function StartSceneByTags(Actor[] akActors, string[] asTags) Global Native     ; GetSceneId recovers the match
int Function StartSceneFiles(Actor[] akActors, string[] asFiles, float afSpeed = 1.0, float afBlendIn = 0.4) Global Native

; --- control (take a handle) ---
bool Function StopScene(int aiScene) Global Native
bool Function AdvanceScene(int aiScene) Global Native              ; take the current node's default edge
bool Function NavigateScene(int aiScene, string asEdgeId) Global Native   ; take a named branch edge of the CURRENT node

; --- state (take a handle) ---
string Function GetSceneId(int aiScene) Global Native
string Function GetSceneNode(int aiScene) Global Native            ; current node id, "" if none
int    Function GetSceneStage(int aiScene) Global Native           ; linear scenes only (else -1)
bool   Function SetSceneStage(int aiScene, int aiStage) Global Native   ; linear scenes only (§1.4)

; --- edges: indexed getters because each edge pairs two fields (id+label); see 1.6 on arrays ---
int    Function GetSceneEdgeCount(int aiScene) Global Native
string Function GetSceneEdgeId(int aiScene, int aiIndex) Global Native
string Function GetSceneEdgeLabel(int aiScene, int aiIndex) Global Native

; --- discovery (inspect candidates; result order matches StartSceneByTags ranking) ---
string[] Function FindScenes(int aiActorCount, string[] asTags) Global Native

; --- scene-metadata introspection (read-only; by scene id, not handle; "" / 0 / empty if unknown) ---
string[] Function GetSceneRoles(string asId) Global Native
string   Function GetSceneRoleGender(string asId, string asRole) Global Native   ; "male"/"female"/"any"
int      Function GetSceneActorCount(string asId) Global Native
string[] Function GetSceneTags(string asId) Global Native

; --- actor convenience ---
int  Function GetSceneForActor(Actor akActor) Global Native       ; 0 if not in a scene
bool Function StopSceneForActor(Actor akActor) Global Native
```

> **Pre-1.0 note — the one allowed breaking change.** Today's `StartScene`/`StartSceneFiles` return
> `bool` and `StartSceneByTags` returns `string`; the handle-returning forms above **replace** them
> under the **same names**. This is safe because OSF has **not shipped publicly** — there are no
> external compiled Papyrus consumers to break, and the in-repo callers (`SAF.psc`/`SAFScript.psc`/
> `OSFTest.psc`) recompile in lockstep (Papyrus auto-casts the returned `int` handle to `bool` in
> boolean contexts, so `If OSF.StartSceneFiles(...)` still reads as "started"). Kept as one clearly
> marked pre-1.0 break rather than permanent `*Handle` aliases. Tier-0 unaffected.

### Inbound arguments

All inbound array parameters (`Actor[]`, `string[]`) must be **non-`None`** — pass a real,
possibly-empty array. A `None` array **asserts in the CLSF binding layer before the native body
runs** (known CLSF limitation; see AGENTS.md "None-array footgun"), so the runtime cannot turn it
into a clean failure — it is a caller contract violation, not a fail-safe path. *Empty* arrays and
other bad inputs (mismatched lengths, `None` elements, unknown ids) are validated in the native body
and return the documented sentinel instead of asserting: `Start*` → `0`, `FindScenes` → empty array,
`ValidateScene` → `false`.

### ID resolution

`StartScene`'s `asId` resolves against **one case-insensitive namespace shared by the scene and
animation-pack registries**:

1. `"scene:author.foo"` forces the scene registry; `"anim:author.foo"` forces the animation-pack
   registry.
2. A bare id resolves **scene registry first, then animation-pack registry** (a composed scene wins
   over a same-named pack entry).
3. A **cross-registry collision is logged loudly at load** (both sources named); authors avoid it by
   namespacing (`author.scenes.*` vs `author.pack.*`), and the prefix is the override.
4. Duplicate ids *within* a registry are **first-load-wins, logged loudly** (the existing pack rule).

A bare pack id still plays, because a linear pack is exposed as a single-path scene (§1.4) — so
existing `StartScene(packId)` content keeps working.

### Start-call argument contracts

**Actor exclusivity (v1):** an actor may be in **one live scene at a time**. Any `Start*` call fails
(`0`) if a supplied actor is already in a live scene — the caller must `StopScene` it first. This
keeps `GetSceneForActor` single-valued and animation ownership/cleanup unambiguous. (Multi-scene
membership is deferred.) A `None` actor, or the same actor passed twice in one call, also fails.

- **`StartScene`** — `akActors` fills definition slots by order (or per the node's `slots`, §1.4).
- **`StartSceneAt`** — like `StartScene`, but the scene's world anchor is `akAnchor`'s position +
  heading (participants co-locate at the ref + each placement offset) instead of `akActors[0]`'s
  transform. `afHeadingDeg < 0` uses the ref's own heading; otherwise it is a heading in degrees. For
  furniture/bed/marker encounters that belong to a *thing*, not an actor — it removes the caller-side
  `MoveTo` + settle dance. Same id resolution as `StartScene` (composed scene def, else pack). A
  `None` anchor fails (`0`).
- **`StartSceneRoles`** — `asRoles.Length` must equal `akActors.Length`; unknown role name,
  duplicate role name, `None` actor, duplicate actor, missing required role, or an extra actor with
  no matching role → start fails (`0`), reason logged.
- **`StartSceneByTags`** — candidate selection is deterministic, the final tie-break random (for
  scene variety): (1) actor count and role filters must match; (2) **every requested tag must
  match**; (3) higher scene `priority` wins; (4) **random among the remaining top tier**. The
  candidate set and ranking are deterministic; only the last step is random. Inspect candidates
  first with `FindScenes(actorCount, tags)` (same ranking order) when a caller wants to choose
  itself. **Actor→role binding:** `StartSceneByTags` keeps the existing gender-fit matchmaking via a
  **deterministic greedy assignment** — actors in array order are each placed into the first
  still-unfilled role whose `filters` they satisfy; if any role is left unfilled the candidate is
  rejected. (`StartScene` by explicit id binds by declaration order with no permutation;
  `StartSceneRoles` is explicit named binding.)
- **`StartSceneFiles`** — creates a **synthetic single-node scene**: `GetSceneId` →
  `"runtime.files:<handle>"`, `GetSceneNode` → `"main"`, no branch edges, `GetSceneStage` → `0`, loop
  mode `hold`. `asFiles` maps **one clip per actor** (`asFiles[i]` on `akActors[i]`, equal lengths —
  it is *not* a per-actor sequence); actors are co-located at `akActors[0]` and synced. It fires
  `NODE_ENTER` on start and, because it holds, `NODE_EXIT` + `SCENE_END` on `StopScene` (it never
  auto-ends). `GetCurrentAnimation(actor)` returns that actor's file (Tier-0 behavior). Per-file
  metadata defaults (sound/equipment) apply where a referenced file has pack metadata; bare ad-hoc
  files get none. Any invalid/missing file fails the whole start (`0`, file named) — a synced scene
  cannot run a partial cast.

### Callback contract

Token-based registration; the payload arrives as an **`OSFEvent:SceneEvent` struct argument** —
read fields directly (`akEvent.node`). **Prototype-confirmed (resolves §2.6 Q1):** there is no
synchronous C++→Papyrus path — CLSF's `DispatchMethodCall`/`DispatchStaticCall` queue a VM stack and
return, so the receiver runs *later*, off the firing stack. Dispatch-time getters are therefore
**infeasible**; the event is snapshotted into the struct. (Async dispatch also makes callbacks
non-reentrant for free — see Semantics.)

```papyrus
; aiScene = 0 means "all scenes"; aiEventMask filters. Returns a generational token (0 = failed).
; One script may register many; each returns a distinct token.
int  Function RegisterSceneCallback(ScriptObject akReceiver, string asFn, int aiScene = 0, int aiEventMask = 65535) Global Native
bool Function UnregisterSceneCallback(int aiToken) Global Native

; The receiver takes one OSFEvent:SceneEvent struct:
Function OnSceneEvent(OSFEvent:SceneEvent akEvent)   ; the function name is the caller's choice
    If akEvent.eventType == OSF.EVENT_NODE_ENTER()
        Actor a = akEvent.actorRef
        ; ...
    EndIf
EndFunction
```

`OSFEvent:SceneEvent` fields: `sceneHandle` · `eventType` · `node` · `edge` · `cue` · `actionType` ·
`actorRef` · `role` · `loopIndex` · `time` · `anchor` · `result`. Three are renamed to dodge Papyrus
reserved-word / type-name clashes (struct members can't equal a keyword or type name,
case-insensitive): **`eventType`** not `event`, **`sceneHandle`** not `scene` (vanilla `Scene` type),
**`actorRef`** not `actor` (`Actor` type).

Event types (mask bits; `EVENT_ALL` = all): `EVENT_NODE_ENTER`, `EVENT_NODE_EXIT`, `EVENT_CUE`,
`EVENT_ACTION`, `EVENT_ACTION_FAILED`, `EVENT_SCENE_END`, `EVENT_SCENE_ABORT`.

Semantics:
- Scope = global with an optional per-handle filter; one script → many registrations, each its own
  token; `Unregister` removes exactly that token (stale token → `false`).
- **Dispatch order** = registration order. **Callbacks are not reentrant** — because dispatch is
  async, each receiver runs on its own *later* VM stack, after the firing native has returned. So a
  callback that calls `Stop`/`Advance`/`Navigate` is naturally safe: it acts on a later tick, and a
  now-dead scene handle reads as invalid for any still-queued callbacks (generational handles). No
  explicit defer queue is needed for the v1 surface.
- **Receiver `fn` takes one `OSFEvent:SceneEvent`** on `akReceiver` (global or member). A missing
  function, or an unloaded/`None` receiver, is **skipped with a warning** (never asserts) and dropped.
- For a named-anchor event, `akEvent.time` is `-1.0` and `akEvent.anchor` names it
  (`"enter"` also reports `time` 0.0; `"exit"`/`"end"` → `-1.0`).
- `EVENT_CUE` → `akEvent.cue` = the cue id. `EVENT_ACTION` → `akEvent.actionType` = a custom
  (non-`osf`) type (best-effort notification, §1.3). `EVENT_ACTION_FAILED` → `akEvent.actionType` =
  the failed built-in `osf.*` type, `akEvent.result` = a `RESULT_*` code (not emitted for custom
  actions in v1).
- The callback token is a distinct, generational handle namespace from scene handles; validated and
  fails safely if confused.
- **String fields** (`node`, `edge`, `cue`, `anchor`, `role`, `actionType`) are `BSFixedString`-
  interned and therefore **case-insensitive** — the returned casing may differ from the authored
  casing (verified in the Phase-A test: `"main"`→`"Main"`). Compare with Papyrus `==` (already
  case-insensitive); never do case-sensitive matching on them.

### Event & result constants

Exposed as **global getter functions** on `OSF` (`OSF.EVENT_NODE_ENTER()`, …) — **confirmed in
build**: a `Native` script's properties cannot be read on the type, so functions are the mechanism
(the `()` is required). Event bits compose into a mask:

| Constant | Value | | Constant | Value |
|---|---|---|---|---|
| `EVENT_NODE_ENTER` | `0x01` | | `EVENT_SCENE_END` | `0x20` |
| `EVENT_NODE_EXIT` | `0x02` | | `EVENT_SCENE_ABORT` | `0x40` |
| `EVENT_CUE` | `0x04` | | `EVENT_ALL` | `0xFFFF` |
| `EVENT_ACTION` | `0x08` | | | |
| `EVENT_ACTION_FAILED` | `0x10` | | | |

Result codes (`akEvent.result`): `RESULT_OK`=0, `RESULT_BAD_ROLE`=1, `RESULT_RUNTIME_FAILURE`=2,
`RESULT_NO_HANDLER`=3. **Disabled-by-user-settings is a silent skip, not a failure** — a suppressed
action emits no `EVENT_ACTION_FAILED` and logs at debug only (the user disabling a mechanism is
expected, not an error). `RESULT_DISABLED_BY_SETTINGS`(=4) is reserved for a future opt-in
"tell me what was suppressed" channel.

### Callback payload by event

Struct fields not listed for an event hold their null/sentinel (`""` / `None` / `-1` / `-1.0`).

| Event | node | edge | cue / actionType | actorRef / role | time / anchor | result |
|---|---|---|---|---|---|---|
| `NODE_ENTER` | new node | entering edge or `""` | — | — | `-1.0` / `"enter"` | — |
| `NODE_EXIT` | leaving node | chosen edge | — | — | `-1.0` / `"exit"` | — |
| `CUE` | node | — | `cue`=id | role on the cue if any | the cue's coordinate | — |
| `ACTION` | node | — | `actionType`=custom type | role if any | the entry's coordinate | — |
| `ACTION_FAILED` | node | — | `actionType`=failed `osf.*` type | role if any | the entry's coordinate | `RESULT_*` |
| `SCENE_END`/`SCENE_ABORT` | last node | — | — | — | — | `RESULT_*` on abort |

(`akEvent.sceneHandle` is the handle for every event; `akEvent.loopIndex` is the current loop.
`akEvent.actorRef` is a real `Actor` object on events that resolve a role — in v1 that is
`EVENT_ACTION` (custom action) with a `role`, where it is the bound participant; other events leave it
`None`. Marshalled via CLSF's handle-policy `PackVariable`.)

## 1.3 The `*.scene.json` schema (v1)

Scene files live in `Data/OSF/**/*.scene.json` and reference animation ids from packs
([PACK_SCHEMA.md](PACK_SCHEMA.md)). The reader tolerates `//` comments and trailing commas (like the
pack reader); canonical/tool-emitted files are strict JSON. Examples are JSONC for readability.

### Minimal scene (the common case — no tracks)

```jsonc
{
  "schema": 1,
  "id": "author.scenes.barflirt",
  "name": "Bar Flirt",
  "priority": 0,                  // optional int, default 0; higher wins in StartSceneByTags ranking
  "tags": ["paired", "social"],
  "roles": [ { "name": "lead", "gender": "any" }, { "name": "other", "gender": "any" } ],
  "entry": "main",
  "nodes": [
    { "id": "main",   "anim": "author.pack.main", "loop": { "mode": "hold" },
      "edges": [ { "id": "finish", "label": "Finish", "to": "climax", "when": "advance", "default": true } ] },
    { "id": "climax", "anim": "author.pack.peak", "loop": { "mode": "count", "count": 3 },
      "edges": [ { "to": "cooldown", "when": "loops" } ] },
    { "id": "cooldown", "anim": "author.pack.winddown", "loop": { "mode": "once" } }
  ]
}
```

### Roles

`gender` (`"male"`/`"female"`/`"any"`) is first-class sugar matching the pack schema; it desugars to
`filters.gender`. `filters` is the open extension point for future actor constraints.

```jsonc
"roles": [ { "name": "lead", "gender": "any", "filters": { /* future constraints */ } } ]
```

**All roles are required in v1** — every declared role must be filled, and every actor must match a
role. Optional roles are deferred (they interact with slot counts, per-node `slots`, fallback
metadata, and tag matching).

### Node

```jsonc
{
  "id": "main",                  // unique within the scene, required
  "anim": "author.pack.main",    // referenced animation id, required
  "slots": ["lead", "other"],    // optional explicit role->slot map (else declaration order)
  "loop": { "mode": "hold" },    // object form only: {"mode":"once"} | {"mode":"hold"} | {"mode":"count","count":N}
  "timerSec": 12.0,              // optional; arms a "timer" edge; NOT inside loop
  "loopForever": false,          // declare an intentional non-terminating hold node (suppresses the warning)
  "tracks": { /* optional */ },
  "edges": [ /* see below */ ]
}
```

**Terminal behavior:**
- `once` + no outgoing edge → ends when the clip ends.
- `count` + no outgoing edge → ends after `count` loops.
- `hold` + no `advance`/`timer`/`trigger` edge → valid but **never ends on its own**; validation
  **warns** unless the node sets `"loopForever": true` (intent declared).

### Tracks (optional decoration)

Closed v1 set of named lanes: `sound`, `cue`, `action`, `camera`.

```jsonc
"tracks": {
  "sound":  [ { "at": 0.35, "repeat": "loop", "pool": "sfx_main" } ],
  "cue":    [ { "at": "end", "id": "finished" } ],
  "action": [ { "at": "enter", "type": "osf.control.lock", "role": "lead" },
              { "at": "enter", "type": "osf.fade.out" },
              { "at": 0.2,     "type": "osf.fade.in" } ],
  "camera": [ { "at": "enter", "state": "orbit_slow" } ]
}
```

| Track | Payload | Meaning |
|---|---|---|
| `sound` | `pool`/sound id, optional `role` | play a sound |
| `cue` | `id` (opaque) | fire `EVENT_CUE`; drives `trigger:<id>` edges |
| `action` | `type` (namespaced) + params | run a built-in mechanism (below) |
| `camera` | `state`/move id | set camera state, **held until the next camera entry, auto-restored on cleanup** |

**Time model** — `at` is the coordinate, `repeat` the recurrence:
- `"at": f`, `0 ≤ f < 1` — clip-local fraction.
- `"at": "enter" | "exit" | "end"` — named lifecycle anchors (use these, not `0.0`/`0.999`).
  `"enter"` fires once on node entry; `"end"` once at the authored clip end of the **first loop**,
  before any terminal transition (so a cue at `"end"` and an `end` edge never "race"); `"exit"`
  fires **exactly once on the actual node exit, whenever it happens** — a `hold` node advanced after
  ten loops still fires its `exit` entries once. The "first-loop-only" rule applies to `enter`/`end`
  and to numeric `repeat:"none"` events — **not** to `exit`.
- Named anchors are **`repeat:"none"` only** — they cannot use `repeat:"loop"`. For a recurring
  per-loop event use a numeric `at` with `"repeat":"loop"`.
- `"repeat": "none"` (default; first loop only) | `"loop"` (every loop; requires numeric `at` in
  `(0,1)`). `at: 1.0` is invalid (use `"end"`). No `atSec` at runtime — tooling converts seconds.
- **Same-tick ordering across lanes is fixed** (not JSON key order): `action` → `camera` → `sound` →
  `cue`. Within one lane, declaration order. So on an `enter` tick a fade/lock action and the camera
  set apply before sounds and cues fire, and a `cue` arming a `trigger` edge is evaluated after the
  tick's track entries have run.

**Built-in actions** (`type` namespaced; `osf.*` are content-neutral mechanisms):

| `type` | Mechanism |
|---|---|
| `osf.equipment.hide` / `osf.equipment.restore` | strip / restore slots (`slots`, `role`) |
| `osf.weapon.sheathe` / `osf.weapon.restore` | holster / re-draw the role's weapon (`role`) |
| `osf.control.lock` / `osf.control.release` | freeze / restore player control + camera (`role`) |
| `osf.fade.out` / `osf.fade.in` | screen fade (`"hold": true` on `fade.out` = end-faded, opts out of auto fade-in) |
| `osf.voice.play` | explicit scheduled vocal (`role`, `set`); default vocals are metadata-driven |

(Camera is a **track only** in v1 — there is no `osf.camera.set` action. Held-state + auto-restore
fit a lane better than a one-shot dispatch; one representation = less surface.)

Per-action payload (required vs optional fields):

| Action | Required | Optional | Notes |
|---|---|---|---|
| `osf.equipment.hide` | `role` | `slots` | unknown role → reject; empty `slots` = the metadata/default profile |
| `osf.equipment.restore` | `role` | `slots` | restores only this scene's ledger entries |
| `osf.weapon.sheathe` | `role` | — | unknown role → reject; holsters the role's weapon |
| `osf.weapon.restore` | `role` | — | re-draws weapons this scene sheathed |
| `osf.control.lock` | `role` | — | unknown role → reject |
| `osf.control.release` | `role` | — | releasing an unowned lock → no-op |
| `osf.fade.out` | — | `hold`, duration | `hold:true` opts out of the cleanup fade-in |
| `osf.fade.in` | — | duration | clears fade ownership |
| `osf.voice.play` | `role`, `set` | — | missing/unknown `set` → reject |

Unknown action handling:
- Unknown **`osf.*`** type → **scene rejected at load** (loud; a silently-skipped policy action is
  dangerous).
- Custom namespaced type (`author.*`, etc.) → emitted as **`EVENT_ACTION`** (distinct from
  `EVENT_CUE`), a **best-effort notification only** in v1: the runtime broadcasts it and continues
  (warns if no listener is registered). The runtime cannot know whether a listener handled it, so
  **`"required": true` is reserved and rejected on custom actions in v1** — a real action-handler
  API with acknowledgment is deferred (§1.7).
- Unknown **fields** → ignored (forward-compatible).

### Edges

```jsonc
{ "id": "finish", "label": "Finish", "labelKey": "$OSF_Finish", "to": "climax", "when": "advance", "default": true, "priority": 0 }
```

- `to` (required) — target node id, or `"$end"` to end the scene (no magic numbers).
- `id` + `label` **required on branchable (`when:"advance"`) edges**; auto-edges may omit them.
  `labelKey` is an optional localization token (e.g. a `$`-string); `label` is the required
  fallback. `GetSceneEdgeLabel` returns `labelKey` when present (the UI layer resolves the token),
  otherwise `label`.
- `priority` — optional int, default 0; higher wins in edge arbitration.
- `default: true` — the edge `AdvanceScene` takes; explicit, never inferred. One per node.
- `when` (v1): `"end"` · `"loops"` · `"timer"` · `"advance"` · `"trigger:<cueId>"`.
  **`cond:<expr>` is deferred** (named predicates via a future registry).

**Edge ids** are unique within a node, may repeat across nodes; `NavigateScene` resolves against the
**current node only**.

**`GetSceneEdge*` exposes only branchable `when:"advance"` edges of the current node** — auto-edges
(`end`/`loops`/`timer`/`trigger`) are never returned. The default `advance` edge is included (it has
an id/label).

**Edge arbitration** (deterministic; not over-strict): if several auto-edges are ready on one tick,
the group order is **trigger → timer → loops → end**; within a group, higher `priority` wins, then
declaration order. Validation **warns** (does not reject) on same-group same-`priority` ambiguity.
Rejection is reserved for un-runnable cases (edge to a missing node, etc.).

## 1.4 Linear scenes, stages & roles

`GetSceneStage`/`SetSceneStage` are defined **only for linear scenes** — auto-converted legacy packs
(stage *i* → node *i*) and scenes that declare an explicit map; on any other graph they return
`-1`/`false`, never inferred from topology:

```jsonc
"linearStages": ["main", "climax", "cooldown"]
```

Role→slot binding: a node's `slots` maps role names to that animation's slots explicitly; absent,
binding is by declaration order. `StartSceneRoles` binds actors to named roles at start.

## 1.5 Lifecycle, events & cleanup

### Transition ordering (canonical)

Per node and per transition, steps run in this fixed order:

```text
Enter a node:
  bind actors to slots → start/blend the animation → fire NODE_ENTER → run "enter" track entries
Leave a node (edge chosen):
  run "exit" track entries → fire NODE_EXIT → stop/transition the animation → enter target OR terminate
Terminate the scene:
  run the final node's "exit" entries + NODE_EXIT (if not already) → replay the undo ledger →
  fire SCENE_END / SCENE_ABORT (handle still valid, read-only) → invalidate the handle
```

(`"end"` track entries fire during the node's final loop *before* the leave sequence — see the time
model. The termination event-order below is the tail of this same sequence.)

### Termination → event mapping

| Cause | Event |
|---|---|
| advance to `$end` / terminal completion | `EVENT_SCENE_END` |
| `StopScene` by a consumer (requested cancellation) | `EVENT_SCENE_END` |
| actor unload · animation failure · fast-travel/cell change · save/load `StopAll` · runtime failure | `EVENT_SCENE_ABORT` |

Consumers can thus distinguish expected completion from forced cleanup. **Event order on
termination:** `EVENT_NODE_EXIT` → undo-ledger rollback (§below) → `EVENT_SCENE_END`/`ABORT` (handle
still valid, read-only, during dispatch) → handle invalidated. (Cleanup runs *before* the END/ABORT
event so a listener reacting to scene-end sees actors already restored.)

### Undo ledger (cleanup is a runtime guarantee)

Reversible built-in actions are scoped to the scene handle and tracked in a per-handle undo ledger;
cleanup never depends on an authored terminal action.

- On **any** termination the ledger replays **exactly once**, reverse order, **idempotently**.
- Authored `osf.equipment.restore` / `osf.control.release` are timing optimizations, not the safety
  net.

Per-mechanism ownership:
- **Control locks** — ref-counted by owning scene handle. Two scenes locking → count 2 (both must
  release); a scene locking twice → released once on its termination; releasing a lock not owned →
  clamped no-op; `StopScene` releases all of that handle's locks; the player unlocks at count 0.
- **Equipment** — the ledger records only items *this* scene hid; restore touches only those; items
  the player/another mod changed mid-scene are not blindly overwritten.
- **Weapon** — the ledger records the actors *this* scene sheathed; cleanup re-draws exactly those.
  Sheathe/restore are a **symmetric pair** (re-draw is unconditional, like `control.lock`), so author
  `osf.weapon.sheathe` only on a role you know is armed — exactly as you'd `osf.equipment.hide` only an
  actor you mean to strip. A state-aware restore (skip re-drawing an actor that had nothing drawn) is a
  noted future refinement; it needs the `actorState` weapon-drawn bit, unverified on this build (RE.md).
- **Camera** — each camera entry snapshots the prior state; on cleanup the scene restores only if it
  still owns the active camera state (last-writer ownership); concurrent camera owners are resolved
  last-writer-wins with the snapshot chain.
- **Fade** — `osf.fade.out` is reversible by default (abort/cleanup fades back in); a scene that
  intends to end faded-out sets `"hold": true` (or hands fade to the caller), opting out of the
  auto fade-in.

### Settings precedence

`effective = (scene/metadata wants) AND (user settings allow)` — **user/global safety settings
always win.** Explicit scene actions override pack-metadata defaults *unless* a user setting
disables that mechanism; metadata supplies a default only when the scene specifies no explicit
action/profile.

## 1.6 Validation & diagnostics

Loading is never fatal; bad scenes are skipped and reported.

- **Required:** `schema`, `id`, `entry`, `nodes`; each node `id`+`anim`; each edge `to`; branchable
  edges `id`+`label`. A `.scene.json` missing `schema` is **rejected** (no silent defaulting).
- **Ids:** case-insensitive, recommended `[A-Za-z0-9._-]`. Duplicate scene id across files →
  first-load-wins, logged loudly. Duplicate node id within a scene → reject.
- **Refs:** edge `to` a missing node, or `anim` referencing a missing animation id → reject (id
  named). Unreachable node → warning.
- **Loop:** `loop.mode` required; `count` mode needs integer `count > 0`; unknown mode → reject.
- **Times:** numeric `at` in `[0,1)`; `at:1.0` invalid; `repeat:"loop"` needs numeric `at` in
  `(0,1)`; named anchors may not use `repeat:"loop"`. Multiple events may share a time (declaration
  order).
- **Timers:** `timerSec` with no `timer` edge → warning; `timer` edge with no `timerSec` → reject;
  `timerSec <= 0` → reject; multiple `timer` edges allowed only under priority/declaration-order.
- **Impossible edges:** `when:"end"` on a `hold` node → reject; `when:"loops"` on a `once` or `hold`
  node → reject (no loop count to reach). (`when:"timer"` without `timerSec` already rejects.)
- **Trigger edges:** a `when:"trigger:<cueId>"` edge must reference a `cueId` emitted by a `cue`
  track entry on the **same node** → else reject. (Cross-node / external cue sources are reserved
  for a later version, which would relax this to a warning.)
- **Edges:** duplicate `advance` edge `id` within a node → reject; more than one `default:true`
  `advance` edge → reject. A node may have branchable edges with no default, but `AdvanceScene` then
  returns `false` — a default is never inferred.
- **Roles:** all roles required in v1; every declared role must be fillable.
- **Priority:** `priority` is an optional int (default 0); non-int → reject.
- **Unknown:** unknown **track-lane name** → reject; unknown **`osf.*` action type** → reject;
  unknown **custom-namespaced action** → `EVENT_ACTION` (best-effort, §1.3); unknown **fields inside
  known objects** → ignored.

Diagnostics API (returns `string[]` — this is safe and intentional: the None-array footgun is about
*inbound* `None` arrays asserting in the binding layer; *returning* a real, possibly-empty array is
fine. Indexed getters are used only where an item pairs multiple fields, e.g. edges' id+label):

```papyrus
int      Function ReloadPacks() Global Native            ; reloads BOTH animation packs AND scene files; returns count
string[] Function GetSceneLoadErrors() Global Native     ; problems from the last (re)load
bool     Function ValidateScene(string asId) Global Native
string[] Function GetSceneValidationErrors(string asId) Global Native
```

(`ReloadContent` is provided as a clearer alias of `ReloadPacks`; both reload packs + scenes.)
**Reload aborts live scenes:** a reload first ends every live scene (`EVENT_SCENE_ABORT` + ledger
rollback, like `StopAll`) before swapping the registries, so no live scene ever references a stale
definition.

## 1.7 Versioning & stability

- Native signatures freeze at **OSF 1.0** (semver; [API.md](API.md)) — *after* implementation phases
  A+B+C land and are tested (§2.5). **A+B+C are now done + tested (2026-06-17)**, so the surface is at
  its freeze candidate; treat Part-1 signatures as stable from here (the one allowed pre-1.0 break,
  `StartScene*`→int handle, already landed).
- The scene schema carries its own `schema` int, versioned independently; unknown fields are
  additive by design.
- **Deferred from v1** (named so authors don't depend on them): the Papyrus scene builder (Papyrus
  is run-only; scenes are file-based) · `cond:<expr>` / condition registry · arbitrary
  `SetSceneNode` on graphs · scene-level `policy` object (all policy is `action` entries) · `atSec`
  runtime field · `osf.camera.set` action (camera is a track) · free-fly/orbit/matrix camera states
  (v1 camera lane = `thirdperson_hold` only) · pool/set→clip metadata resolution (v1 sound/voice
  take a literal spec) · positioned Wwise posting · the `equipment.hide` `slots` filter (v1 strips
  all worn apparel) · per-role `equipment.restore` · a custom action-handler API with acknowledgment
  / `required` custom actions (v1 custom actions are notification-only) · optional roles · the C-ABI
  builder · scene parameters / auto-drive (§2.4).

---

# PART 2 — INTERNAL DESIGN *(non-contract; may change freely)*

## 2.1 Why one mod, with an internal seam

The scene runtime + policy fold into OSF Animation as an internal subsystem (one DLL, one mod). The
earlier three-plugin split cleaned the code but split one product across two deploys for no user
gain; its technical point — engine-fragile surface behind one boundary — is preserved as an
**internal module seam**. The pre-split policy systems (`Adjust/Audio/Camera/Equipment/Config`) are
intact in `ARCHIVE/OSF-Animation-Public`. (Rationale: the design memory + [INTIMACY_SEAM.md](INTIMACY_SEAM.md).)

## 2.2 Layering

| Layer | Responsibility | Touches |
|---|---|---|
| **A — Playback core** | the two engine hooks, rig stamping, anchor/pin, clocks. = Tier 0. | the **fragile animation playback hooks + animation-runtime state** |
| **B — Scene runtime** | graph execution, navigation, the track scheduler, the undo ledger, callback dispatch, the handle table, command queue. | Layer A's surface; Layer C for actions |
| **C — Mechanism services** | Equipment/Sound/Fade/Camera — content-neutral *how*, no scene knowledge. | **stable game-facing services** via CLSF |

**Invariant:** only Layer A touches the fragile animation playback hooks and animation-runtime
state; Layer C may call stable game-facing services (equipment, audio, fade, camera). Two layers
touch "the game"; only one touches the patch-fragile part.

## 2.3 Two registries

Animation registry (`*.json` packs, reusable building blocks) + scene registry (`*.scene.json`
graphs, reference anim ids). A linear pack is auto-exposed as a single-path scene so
`StartScene(packId)` and stage indexing keep working.

## 2.4 Scene parameters / auto-drive *(deferred — content-neutral, data-defined)*

The "excitement system" resolves to a generic feedback mechanism, not NSFW code: a named scalar
(per-actor or per-scene) with rise/decay that modulates speed and gates an edge. "Arousal → peak" is
one data preset shipped with intimate content; the same facility serves SFW uses (dance `energy`,
vignette `tension`). Deferred past v1 (unreviewed; and param-gated edges are a form of condition,
which v1 also defers). It slots into Layer B additively as a `when:"param:<name> >= x"` edge + a
`parameters[]` schema block.

## 2.5 Phasing — implementation order vs the v1 freeze

The Part-1 contract is the **end state**. It did not ship piecemeal; the implementation phases below
built toward it, and **public v1 freezes only after A+B+C are implemented and tested.** So built-in
actions and the undo ledger *are* part of v1 — they landed in phase C. **All three phases are now
DONE + in-game tested (2026-06-17), so this is the v1 freeze candidate.**

| Phase | Scope | Status |
|---|---|---|
| **A** | Layer B scheduler, handle table, command queue, token callbacks, diagnostics, `sound`/`cue` tracks. Linear packs run through it. | ✅ done + tested |
| **B** | Graph navigation: `edges[]`, branches, `Navigate`/`GetSceneEdge*`, arbitration + validation. | ✅ done + tested |
| **C** | Built-in `action`s + Layer C services (equipment/fade/voice/sound/camera) + undo-ledger integration + the `sound`/`camera` tracks + settings precedence + validation. | ✅ done + tested |
| **— v1 freeze —** | A+B+C landed + passed in-game tests → freeze Part 1 and tag 1.0. | ⬅ here now |
| **D (post-v1)** | C-ABI builder; conditions / scene parameters; free-fly/orbit camera; pool/set→clip metadata; positioned Wwise (all additive). | deferred |

## 2.6 Open questions (post-review)

1. ~~**Callback transport**~~ **RESOLVED (Phase-A prototype, 2026-06-17, in-game verified):** no
   synchronous C++→Papyrus path exists; dispatch-time getters are infeasible. The payload is passed
   as an **`OSFEvent:SceneEvent` struct** argument via async `DispatchMethodCall` (per-receiver) /
   `DispatchStaticCall` (debug probe). Constants ship as `OSF.*()` global functions (a `Native`
   script can't expose properties on the type). **Struct-marshalling traps (for any future struct):**
   CLSF's `structure_wrapper::insert` is broken (looks up members with a `std::string_view` against a
   `BSFixedString`-keyed map → hash miss); `varNameIndexMap::find` is *also* unreliable; and the
   compiler **reorders** struct members (declaration order ≠ slot order). The working recipe:
   `CreateStruct` → **iterate** `varNameIndexMap` to build a case-folded name→slot map → write
   `proxy->variables[slot]` directly. See `src/Scene/SceneEventRelay.cpp`.
2. **Camera ownership under true concurrency** — last-writer + snapshot chain (§1.5) is the v1 rule;
   revisit if real multi-scene camera contention appears. **v1 shipped a minimal-safe camera lane**:
   the single content-neutral state `thirdperson_hold` (force + hold third person via the
   ref-counted standalone camera lock, ledger-tracked + auto-restored). Free-fly/orbit/matrix camera
   states are **deferred** — the runtime free-fly gate is still OPEN in OSF RE (`camera.state_machine`;
   `SetCameraState`=113375 statically proven) and the prior orbit attempt crashed/reverted. The
   multi-state snapshot chain (held transitions between several camera states) is deferred with them.

## 2.7 Reconciliation with other docs

- [API.md](API.md) — Tier-0 unchanged; Tier-1 lands additively (note the pre-1.0 `Start*`
  return-type change).
- [INTIMACY_SEAM.md](INTIMACY_SEAM.md) — plugin seam → internal module seam; contract carries
  forward.
- [PACK_SCHEMA.md](PACK_SCHEMA.md) — packs unchanged; vocals/undress defaults live in pack metadata +
  user settings (Layer C), not author-placed.
- `AGENTS.md`, `README.md`, `docs/{API,GETTING_STARTED,PACK_SCHEMA,INTIMACY_SEAM,NEXUS_PAGE,guide/*}.md`
  — reconciled 2026-06-17 from the pre-merge "policy lives in a separate OSF Intimacy plugin" framing
  to the merged reality (scene runtime + content-neutral policy mechanisms are in this repo; specific
  adult content → the OSF Seduce mod). RE.md / POST_PATCH_CHECKLIST.md track Layer-C binding additions
  alongside Phase C.
