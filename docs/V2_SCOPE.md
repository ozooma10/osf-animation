# OSF Animation 2.0: scope of the breaking change

Status: design, not started. Written 2026-10-22. Current release is 1.5.0, scene ABI 1.7,
services ABI 3.17. Nothing here is committed; this document exists to bound a v2 before any of
it starts, and to record what deliberately stays out.

A major version is permission, not a plan. The bar used throughout: **a change earns the break
only if it removes a structural duplication or a permanent two-semantics wart.** Cosmetic
cleanups do not justify a v2; they ride along once the break is happening anyway. Measured
against that bar, two changes qualify (§1, §2) and three are passengers (§3, §4, §5).

## Why now rather than later

In-workspace consumers are OSF Intimacy, OSF Seduce, the Browser extension and the SDK
examples - all ours. No third-party content pack or native plugin is known to ship against the
scene ABI yet; the adult content that exists for Starfield targets SAF/NAF instead (see
[OSF Intimacy's community record](../../OSF%20Intimacy/docs/COMMUNITY_REQUESTS.md)). The cost of
breaking is therefore near its lifetime minimum and rises monotonically from the first day a
third party ships an `*.osf.json` or links the engine API.

The sequencing consequence is the important one: **2.0 lands before OSF Intimacy's first
release**, because Intimacy's sidecar keys, node addressing and provider stream all ride on these
shapes. Shipping Intimacy V1 on 1.x and breaking afterwards means breaking third-party sidecars
twice.

| # | Change | Size | Earns the break? |
|---|---|---|---|
| 1 | Node as the top-level registry entry; routes are data over node ids | L | Yes - removes a dual address and three transports for one operation |
| 2 | Layer stack as the playback primitive (semantics in 2.0, depth 1 implemented) | M spec / L later | Yes - avoids a permanent two-semantics composition wart |
| 3 | Collapse the scene and services ABI tables | S | No - passenger |
| 4 | Retire alias pairs and the linear/graph authoring duality | M | No - passenger |
| 5 | `allowManualHold` becomes navigation policy, not a content grant | S | No - passenger |

Vocabulary follows [VOCABULARY.md](VOCABULARY.md): scene, flow node, linear stage, playback
segment, participant, role, world anchor, world placement. §1 changes what *scene* denotes, and
that document is updated with it.

---

## 1. The node as the top-level registry entry

### The duplication

OSF Intimacy's ecosystem model already decided the node is the unit of composition, meaning and
patching (its E1). The runtime did not follow: the scene is still the container, a node is
addressed as `(scene, node)`, a direct node start retains the containing graph, and one
conceptual operation - *go from this node to that node* - has three implementations:

| Transport | Scope | Constraint |
|---|---|---|
| Authored scene edge | within one scene | none |
| `use` splice | across scenes | target must be a single-flow-node inline-stage scene |
| Covered handoff | across scenes/packs | needs cover, bounded deadline, may refuse |

`linearStages` exists as a fourth addressing scheme layered on top, mapping a linear index onto
flow-node ids for the older stage getters.

OSF Intimacy's model records the consequences as open questions rather than answers: how an
in-pack route compiles to edges, whether authors write routes directly, and general cross-pack
route search are all deferred there. They are deferred because the container, not the node, is
what the API addresses.

### The change

The node becomes the registry entry. A route is authored data over node ids - ordered or
branching - and is not a second container type. One address (a namespaced node id), one
transport (an edge between nodes), with cover required only where a boundary actually needs it.

What this removes:

- the `(scene, node)` dual address and the "direct start retains the containing graph" special case
- the `use` splice and its single-node inline-stage restriction
- `linearStages` and the linear-index mapping
- two executors for in-pack versus cross-pack movement

What it keeps, deliberately: the **cost asymmetry is a fact, not a code path**. A boundary whose
poses match inside one pack is still seamless; one that needs cover still pays a bounded deadline
and may still refuse. After this change that difference is a property of the boundary, reported
through one mechanism, instead of two subsystems with separate failure modes.

### Cost

The Browser extension's catalog is built around scene containment (`CatalogBuilder`,
`BrowserClipLibrary`), so it is part of this change rather than a follow-up. Scene-level
matchmaking, tags, priority and weight need a defined meaning once the node is primary - most
likely as defaults a node inherits from its route, resolved at load.

---

## 2. Layer semantics now, depth 1 implemented

### The wart this avoids

Today one actor has one playback graph: `GraphManager` keys a single `Graph` per actor, and that
graph holds one animation, one clip time and one current file. Partial-body playback exists -
named bone masks with feathered seams, `poseMode: "additive"`, `poseWeight` - but it composes
against the **engine's** pose. A mask releases unlisted bones to engine control, and additive
deltas are taken against the animation skeleton's rest transform and applied over a snapshot of
the live engine base.

That makes a gesture over a *vanilla* locomotion pose expressible, and a gesture over an
*authored OSF* pose not expressible. Mask the right arm during a scene and the rest of the body
falls back to the vanilla idle rather than continuing the authored position.

Adding a second slot later, inside a surface that already shipped, leaves two composition
semantics permanently coexisting - layer-over-engine and layer-over-OSF - with `poseMode`, `mask`
and `poseWeight` meaning different things depending on which slot an author is writing for.

### The change

Specify one rule in 2.0: an actor has an **ordered layer stack**; layer 0 composes against the
engine pose and layer *N* composes against the composite of the layers below it. `poseMode`,
`mask` and `poseWeight` are per-layer and mean exactly one thing each at every depth.

**Implement depth 1.** The 2.0 runtime ships a stack whose depth is one, which is behaviourally
today's single slot. The general rule is in the contract and asserted by the engine-free pose
suites against the depth-1 case, so the actual multi-layer implementation lands later as an
additive 2.x with no further break.

This split is the point of putting it in the breaking window: the *contract* needs the break, the
*implementation* does not. It also keeps the two qualifying changes in different code. §1 is
registry and data model; a multi-layer implementation touches the compose hook, binding, live-base
snapshotting, the stamp probe and the stranded-graph sweep - the most reverse-engineered code in
the plugin, whose invariants currently assume a single slot. Rewriting those in the same branch as
the registry change is how a working mod stops working.

### What the 2.0 spec must settle

Written out in [LAYER_CONTRACT.md](LAYER_CONTRACT.md), which this section requires. In summary:
one write point per frame with the stack composing off-buffer (L0, the prerequisite — the rig slot
array is currently both the engine-base source and the output, and the stamp probe exists to
reject reads of our own write); what a layer's base is at each depth (L1); that a mask releases to
the composite below rather than the engine (L2, the point that keeps depth 1 from being a special
case); per-layer weight composition (L3); clock ownership, including paired gestures and pause
(L4); lifetime, removal and what a blend-out lands on (L5); per-layer versus per-role and
per-participant declarations (L6); the depth-1 equality invariant the suites assert (L7); and
deterministic stack ordering (L8).

The load-bearing finding is that the pose math itself does not change: `WriteAdditive` and
`WriteOverrideTransition` already take the base as a parameter, so layering changes which buffer
that pointer addresses and nothing else. That is what makes specifying now and implementing later
safe rather than speculative.

Capability fields that nothing reads yet follow the existing precedent of a defined-but-unconsumed
region: a diagnostic no-op, never an error, and **absence means deny** rather than permit.

---

## 3. Collapse the ABI tables

Scene ABI 1.0 through 1.7 and services 3.0 through 3.17 become one table each at 2.0 and 4.0.
This also retires the rejected services 3.13 prefix, where two private branches assigned
incompatible functions to the same slot and requests for that minor are refused outright.

Pure robustness, no design risk, and free while breaking. Not a reason to break on its own.

The stability policy itself does not change: from 2.0 on, natives are never removed or
re-signatured within the major version, `OSFTypes:SceneEvent` members append at the end, and the
native API appends vmethods and POD fields in minors.

---

## 4. Retire the alias pairs and the authoring duality

The canonical/legacy pairs lose their legacy spellings: `lockPlayer`, `stripActors`, `inPlace`,
`playerControl`, the prop `node` alias, the `Stage`/`StageData` compatibility names, the `curated`
boolean superseded by `sourceKind`, and the `activeScenes` wire name. Each is a parse branch, a
documentation paragraph and a test case.

Linear scenes stop being a second schema. They are already not a second runtime - the loader
desugars `clip`/`stages[]` into a flow-node chain through `DesugarLinear`, and the synthetic ids
are why `#` is reserved in authored ids - so this is a surface-level removal.

**Keep a one-node-with-stages shorthand.** The easy on-ramp is not a cosmetic feature: the lowest
possible authoring floor is what SexLab's author named as the reason his framework was adopted at
all, and it is the same argument for OSF. Removing two schemas is the goal; removing the short one
is not.

---

## 5. `allowManualHold` becomes policy

A node or linear stage currently opts into manual holding with `allowManualHold: true`, which the
Bridge slice required content to grant. Holding at a boundary is a decision belonging to the
caller's navigation policy, not a property a clip hands out. With the node primary (§1), hold
becomes a boundary policy and the content flag retires.

---

## 6. What does not change

- **Content neutrality.** Animation owns clips, timing, placement, blending, cues and playback;
  semantic vocabulary, poses, acts, ranking and gameplay policy stay in consumers. This boundary
  is the reason a third-party provider can exist at all and is preserved verbatim.
- **The engine-free test suites.** They are what makes any of this checkable without the game
  running, and both qualifying changes are specified so they remain assertable there.
- **The stability policy's shape** (§3), and the per-save persistence C ABI.

---

## 7. Out of scope

Additive work that does not need the break, and must not be allowed to enlarge it: inverse
kinematics on gesturing limbs, aim-at-body-part interaction, beat-locked or analog player input,
semantic act or pose vocabulary of any kind, persistent gameplay state, and a second scene model.
A v2 that absorbs these is a rewrite that does not ship.

---

## 8. Order and evidence

Not started, and not to be started while `osf/paired-animation-placement` is in flight.

| Step | Lands | Done when |
|---|---|---|
| A | §2 spec only: the layer contract written, depth-1 semantics asserted against the general rule | Pose suites assert the depth-1 case through the general composition rule, not a single-slot special case |
| B | §1 registry and addressing, including the Browser catalog | One address and one transport; a route over node ids executes seamlessly in-pack and refuses legibly where cover is needed; scene-level matchmaking has a defined node-level meaning |
| C | §3, §4, §5 | One ABI table each; no legacy spelling parses; the shorthand still authors a single node; hold is a policy decision |
| D | Later, additive 2.x | Multi-layer implementation against the §2 contract, no further break |

Consumers update with B: Intimacy, Seduce, the Browser extension and the SDK examples. OSF
Intimacy's V1 then builds on 2.0 directly, with its one-way decisions designed in rather than
bolted onto a 1.x surface.
