# Actor animation composition and state routing

Status: proposed. This document describes a target architecture; none of the new runtime, API, or
schema names below should be read as implemented behavior. This is the simplified revision: the
procedural IK tier, hierarchical claim registry, capability vocabulary, three-tier enforcement,
two-phase prop handoff, and locomotion blending profiles from the first draft were cut or moved to
the [deferred appendix](#appendix-deferred-and-sketched). A separate candidate RE track investigates
whether engine-side conditional clip replacement can cover restricted locomotion without runtime
pose blending.

Related current contracts: [scene schema](SCENE_SCHEMA.md), [consumer API overview](API.md), and the
authoritative [native scene ABI header](../src/API/OSFSceneAPI.h).

## Decision

OSF should add a per-actor animation runtime beneath `SceneRuntime`, rather than turn every durable
actor condition into a long-running scene.

The runtime accepts several kinds of presentation intent for an actor:

- a short **route** between presentation stations, such as helmet Head -> Held -> Stowed;
- a persistent **condition assertion**, matched to a presentation profile such as front handcuffs or
  an armbinder;
- the actor's role in one OSF **scene**; and
- small compatible gestures or additive layers.

It arbitrates those intents, samples every accepted layer against one captured Starfield pose, and
stamps one final pose. Separately, OSF should investigate whether asserted conditions can drive
engine-side **clip replacement**. If an actor-aware substitution seam is proven, that optional
backend could replace selected locomotion clips without OSF composing those poses itself.

Consumer mods continue to own gameplay truth and policy. OSF owns the temporary animation state
needed to present that truth.

This gives Suit Protocol a cohesive helmet route without making a stowed helmet monopolize the
actor's scene slot. It also gives a restraint mod a path to keep cuffs visible in ordinary
locomotion and in compatible multi-actor scenes without requiring a bespoke animation for every
combination.

## Why this is a separate runtime

The current systems each solve a useful, narrower problem:

| System | What it does today | Important limit |
| --- | --- | --- |
| Starfield animation graph | Produces the live locomotion and gameplay pose | OSF consumes its result; OSF does not control its state machine |
| `Animation::Graph` | Samples and stamps one OSF clip for one actor | Despite its name, it is a clip player, not a high-level state graph |
| `GraphManager` | Owns the animation hooks and one graph per actor | A second independent OSF layer cannot safely stamp the same actor |
| `SceneRuntime` | Runs synchronized multi-actor node graphs, tracks, policy mechanisms, and callbacks | An actor may be in only one live scene, and every graph node needs a playable |
| Role pose policy | Composes one override/additive clip through a mask and weight | The policy is scene-wide for the role, not a general layer stack |
| Scene prop ledger | Creates, attaches, moves, and cleans up temporary render props | All remaining props are destroyed when the scene ends |
| Studio Prop Routes | Authors stations, ownership, endpoint poses, transition clips, and handoff frames | There is not yet a generic runtime asset for the authored route |

The existing scene node graph is still the correct abstraction for a synchronized encounter. It
owns a roster, common clock, placement, camera, sound, actions, and navigation. It is the wrong
owner for an actor's indefinite handcuffed condition or a zero-animation Stowed state.

Conversely, an actor-layer runtime should not grow camera direction, matchmaking, actor placement,
or scene-wide control policy. Those remain `SceneRuntime` responsibilities.

## Goals

1. Preserve Starfield locomotion as the implicit base whenever OSF does not intentionally replace
   it.
2. Let multiple compatible OSF sources affect one actor without multiple systems stamping the rig.
3. Represent zero-animation stable states without keeping a scene or clip alive.
4. Separate a mod's durable semantic state from OSF's reconstructible presentation state.
5. Make interruption, reversal, suspension, and reconciliation explicit.
6. Detect semantic conflicts before two clips happen to fight over the same bones.
7. Support two restraint quality tiers: a generic masked pose over compatible content, and a fully
   authored restrained variant selected by matchmaking.
8. Prefer engine-native locomotion mechanisms; investigate conditional clip replacement before
   committing to any OSF-side locomotion blender.
9. Keep existing scene definitions and APIs working while the lower runtime changes incrementally.
10. Reuse the route model already present in Studio instead of creating a second helmet-specific
    authoring format.

## Non-goals

- OSF does not decide whether a helmet should be sealed, whether a device is equipped, or whether a
  restrained actor is allowed to enter a scene.
- OSF does not persist consumer gameplay state. It may cache presentation state only for the loaded
  world.
- OSF does not change actor movement speed, inventory, AI packages, combat rules, or escape logic.
- This is not a replacement for Starfield's complete locomotion graph.
- Bone masking cannot automatically make a scene physically plausible when the original pose
  depends on a restrained limb for balance or contact; those scenes need authored variants or
  rejection.
- No runtime IK/constraint solving in this design (deferred; see appendix).
- The first implementation does not need cross-actor physics, cloth simulation, or an unbounded
  number of layers.

## Vocabulary

The names matter because `Graph` currently means several different things in the codebase.

- **Semantic state:** Gameplay truth owned by a consumer. Suit Protocol owns Sealed/Unsealed and its
  transactional phase. A device mod owns which restraints are equipped and how restrictive they
  are.
- **Presentation station:** A named visual posture/ownership point within a route. Head, Held, and
  Stowed are helmet stations. A station is not necessarily a gameplay state and may contain no
  animation.
- **Route:** Stations plus directed transitions. A transition can have a clip, blends, ownership
  markers, actions, and interruption rules. The consumer requests a destination; OSF presents the
  path.
- **Condition:** A lowercase, namespaced fact asserted for an actor by a consumer, such as
  `example-restraints.hands.front`. OSF does not discover or persist it. Equipping and removing a
  device may use routes, while the consumer keeps the worn condition asserted.
- **Presentation profile:** A data-driven continuous visual selected by conditions. It contributes
  pose layers, claims, and props. It is not a scene and does not own gameplay truth.
- **Layer:** One contribution to an actor pose: a clip, cached pose, or additive motion. A layer
  declares where it composes, which bones it writes, and which claim targets it holds.
- **Candidate clip replacement:** A proposed condition-gated substitution of the animation file the
  engine itself plays. If the RE track proves an actor-aware seam, this would run inside the vanilla
  pipeline rather than as an OSF pose layer.
- **Claim:** A coarse semantic resource a layer controls, such as `pose.arms` or
  `attachment.hand.right`. Claims prevent combinations that a bone mask alone cannot diagnose, and
  they double as the capability query surface: "can this actor hold something" is answered by
  asking what exclusively claims the hand targets.
- **Scene body layer:** The per-role pose submitted by `SceneRuntime` to the actor runtime. There is
  still at most one live cinematic OSF scene per actor, but compatible persistent actor layers may
  compose with it.

## Ownership rule

Consumer mods own durable meaning. OSF owns disposable presentation.

For every integration, this should answer four questions:

1. Who decides the desired semantic state?
2. Who owns each visible prop at each station?
3. Which authored marker commits a visual or gameplay handoff?
4. How is the presentation reconstructed after load, interruption, or actor 3D replacement?

OSF must never infer that a device has been removed merely because its animation was suspended. A
consumer must never need to serialize an OSF clip time or internal node handle. On a world-replacing
load, OSF clears its actor runtime; consumers restore semantic truth and submit fresh presentation
requests. Load-time reconstruction is the universal repair path for every failure in this document:
any state OSF can lose is state a consumer can cheaply reassert.

## Target architecture

```text
  Suit Protocol       restraint mod       other consumer
  semantic state      equipped devices    semantic state
        |                    |                   |
        +----- desired stations / condition assertions -----+
                                                            |
  Matchmaker + SceneRuntime ----------- scene-role intent ---+
                                                            v
               ActorAnimationService (one controller per actor)
               +-----------------------------------------------+
               | route instances and desired stations          |
               | condition assertions and resolved profiles    |
               | claim arbitration                             |
               | suspension, interruption, and reconciliation  |
               +----------------------+------------------------+
                                      |
                                 accepted layers
                                      v
                             PoseCompositor
          live Starfield pose -> base -> overlays -> restraints
                                      |
                                 one final stamp
                                      |
                                 actor skeleton

  A candidate RE track may add condition-gated clip replacement
  inside the vanilla pipeline before capture; it is not required here.

  SceneRuntime continues to own: roster, sync clock, placement/anchor,
  graph navigation, cue/action/sound/camera lanes, locks, strip/equip, ledger.
```

The working component names are:

- `ActorAnimationService`: lookup, ownership, lifecycle, and the public API;
- `ActorController`: desired/current state and arbitration for one actor;
- `ClipInstance`: the sampling responsibility currently concentrated in `Animation::Graph`;
- `PoseCompositor`: combines all accepted local-pose contributions and stamps once; and
- `ScenePoseAdapter`: lowers a current scene role/stage into a controller-owned layer.

The existing C++ names do not have to be changed in the first phase. The distinction is conceptual:
`Animation::Graph` is a clip sampler, `SceneRuntime` owns scene graphs, and the proposed actor
controller owns per-actor presentation state.

## Per-actor runtime model

Conceptually, one controller contains:

```text
ActorController
|- captured Starfield base pose (owned per-frame scratch)
|- zero or one cinematic scene body layer
|- zero or more route instances
|- zero or more owner-scoped condition assertions
|- zero or more resolved presentation profiles
|- zero or more accepted gesture/additive layers
|- transition/state/controller-scoped prop leases
`- desired, active, suspended, and failed request records by owner
```

Every request has an owner identity. Releasing or unloading one owner removes only that owner's
routes, conditions, profiles, and props. A caller-supplied request token is correlation data only;
OSF assigns its own monotonic instance and transition generations, and a result arriving from a
superseded generation is dropped. Beyond that, delivery relies on the callback contract: reached-
station and ownership-marker callbacks must be idempotent, and a consumer may reassert its current
desired station or condition at any time without duplicating a prop or replaying a completed
transition.

An instance pins immutable route/profile, mask, and skeleton snapshots for its lifetime. Reloading
packs affects new instances or an explicit safe reconcile, never the meaning of an in-flight edge or
its cleanup plan.

A stable station can contain zero layers. When every accepted entry is zero-layer, the controller
has no sampling cost and does not occupy the actor's cinematic scene slot.

### Layer contract

A resolved layer needs at least:

| Field | Purpose |
| --- | --- |
| owner and instance | Isolated cleanup and callback routing |
| source | Clip, static pose, or additive pose |
| phase | Base, overlay, or restraint |
| write mask | Bones and feather weights that the compositor actually changes |
| pose mode and weight | Override or rest-relative additive composition |
| blend policy | Enter, exit, transition, and interruption blends |
| claims | Claim targets held, exclusive or composable |
| enforcement | Required or cosmetic, supplied by the consumer |
| lifetime | Transition, station, profile, controller, scene, or external |
| order | Deterministic visual ordering among compatible layers in the same phase |

Layer order is not request urgency, scene matchmaking priority, or a substitute for compatibility.
If a base scene uses both arms to support the actor, silently sorting a handcuff layer later
produces a broken pose. That combination must instead select an authored variant or be rejected
under the consumer's policy.

Pose layers are only one actor-local resource domain. Root/world placement, movement/AI control,
equipment visibility or protection, and attachment ownership also require explicit claims. Root
placement is exclusive and is not blended as a local-pose layer. Scene-global camera, fade, sound,
and player-control mechanisms remain in `SceneRuntime` and its ledger.

Face/expression channels (gags, blindfolds, pained expressions — heavily used by device mods) are a
foreseen future layer source with their own `pose.face` claim target. They are not designed here;
the layer contract and claim list simply must not paint them out.

## Pose composition

The compositor runs after Starfield has evaluated the actor and before the final rig buffers are
consumed, matching the current hook placement.

Controller mutations, arbitration, props, and public callbacks are game-thread work. The game
thread publishes an immutable pose plan for animation hooks to consume. A hook copies live rig
locals into compose-owned per-frame scratch before any OSF write; it never retains a borrowed rig
buffer pointer in controller state and never rereads a buffer containing OSF's prior stamp. Marker
crossings discovered off the game thread are sequence-stamped and queued for ordered game-thread
dispatch, draining in frame order before new API commands are applied. The reclamation invariant is
that plan publication precedes reclamation: no hook thread may ever read freed plan, clip, or mask
data. The mechanism (shared ownership, epochs, or otherwise) is an implementation choice, not part
of this contract.

For each actor and frame the compositor should:

1. Capture the live Starfield local pose once. This remains the immutable base for the frame. If the
   candidate engine-side replacement track graduates, any replaced clip is simply part of this base.
2. Evaluate the accepted **base** source: ordinary play keeps the live pose; a cinematic scene body
   layer supersedes it on the bones it owns.
3. Apply compatible **overlay** layers, including equipment gestures and upper-body poses. Masks and
   feathered seams determine the numerical blend.
4. Apply accepted **restraint** layers after the scene and soft overlays. A restraint is a
   full-weight masked chain pose; partial weighting is only for entering and leaving it.
5. Convert and stamp the final local pose once.
6. Update render props from the final skeleton transforms.

Within a phase and claim target, accepted layers sort by authored `order` and then a stable
owner/instance key, never by registration timing. Two overlapping, non-commutative writers at the
same phase/order are an authoring error and are rejected; the stable key is only a deterministic
tie-break for non-overlapping work. An override interpolates the accumulated local transform toward
the sampled transform by `transition weight * layer weight * bone-mask weight`. An additive layer
computes rotational `delta = inverse(restRotation) * sampledRotation`, shortest-path blends that
delta from identity by the same weight, and post-composes it onto the accumulated rotation. Its
translation adds `(sampledTranslation - restTranslation) * weight`; additive scale remains
engine/underlay-driven, matching current pose math.

This order is intentional. The profile realizing a handcuff condition should constrain the arms
produced by a compatible scene, not be erased by that scene. An incompatible scene never reaches
this composition step under strict policy.

Independent graph objects must not stamp the same actor in sequence. Doing so makes the last hook
writer win, lets later layers capture already-modified state as their supposed base, and makes the
result depend on registration order. Sampling many layers is valid; stamping many final poses is
not.

### Crossfades

Crossfades belong to each layer or transition, not to the whole actor. On a layer change, the
compositor retains that layer's previous sampled contribution when rigs match. When a layer fades
out or is suspended, its target is the newly recomposed pose beneath it, not a stale snapshot of the
whole actor.

The existing role-level `mask`, `preserveBones`, `poseMode`, and `poseWeight` map directly into the
new layer contract. Masks should eventually be data-defined and skeleton-family-aware rather than
limited to a hard-coded table.

### Skeleton compatibility

Every sampled asset and actor binding needs a skeleton-family, topology, canonical-bone-map, and
rest-pose fingerprint. A mask includes the required ancestor closure needed to evaluate its model
transforms. Crossfading or composing clips directly is allowed only when those fingerprints are
compatible; otherwise the definition must name an explicit retargeter or fail before playback.
Equal joint counts are insufficient. In particular, a rest-relative additive layer cannot use a
different bind/rest pose merely because its bone names happen to match.

## Routes and state requests

A route is a presentation state machine, but its trigger policy remains outside the asset.

The controller API should accept a desired station rather than require a consumer to manually walk
scene nodes. Given a request such as `RequestStation(instance, "stowed")`, the controller finds an
authored path from the current reached station/checkpointed ownership state, plays its transitions,
and reports typed events:

- transition started;
- authored marker reached;
- station reached;
- transition interrupted or superseded; and
- request failed with a reason.

A route instance records these separately:

- the latest desired destination;
- the last fully reached station;
- the active edge and pose progress;
- the most recent irreversible ownership/gameplay checkpoint; and
- OSF's internal transition generation.

This distinction matters when an ownership transfer happens before the destination pose is reached.
The route cannot pretend it is still wholly at the source station, but it also has not completed the
destination.

Before an edge begins, the controller pins and validates its clip/layer binding, reserves every
transition claim, retains a compensatable source, and acquires the resources needed to reach the
destination checkpoint/station. A multi-edge request may acquire one edge at a time only when each
intermediate station is independently valid and can safely wait there. A path containing a purely
transient intermediate must reserve the whole uninterrupted segment. The runtime must never cross
an ownership checkpoint and only then discover that the promised destination cannot be realized.

Transitions define their own reversal and commit behavior. A countermand before the ownership
marker may reverse immediately; after a one-way handoff it may need to finish the current transfer
and follow the reverse edge. The route asset describes what is visually safe. The consumer decides
which destination is now desired.

The initial interruption vocabulary can stay small: `complete`, `reverse` when an authored reverse
edge exists, `crossfade-to-latest`, and `non-interruptible`. Requests received while suspended still
replace the desired destination, but they do not mutate unavailable resources. On resume the route
reconciles from its checkpointed ownership and latest completed station rather than continuing an
obsolete clip clock. `crossfade-to-latest` is valid only for a pose-only edge or before its first
irreversible checkpoint; otherwise the route must finish, compensate, or follow an authored reverse
edge.

Timers are policy inputs, not a classification of the animation. Suit Protocol may request Stowed
three seconds after Held, or leave Held active until another input. Both use the same route. A held
station may therefore remain indefinitely without becoming a cinematic scene.

### Illustrative route asset

This is a design sketch, not an accepted registry schema. It deliberately follows Studio's current
station/transition vocabulary. Reverse edges and full socket/body-pose data are omitted for space.

```json
{
  "schema": "draft/osf-actor-runtime/0",
  "kind": "actor-route",
  "id": "suit.helmet",
  "fps": 30,
  "stations": [
    {
      "id": "head",
      "ownership": {
        "kind": "equipped-armor",
        "owner": "consumer",
        "lifetime": "external"
      },
      "layers": []
    },
    {
      "id": "held",
      "ownership": {
        "kind": "carrier",
        "bone": "R_AnimObject1",
        "owner": "osf",
        "lifetime": "station"
      },
      "layers": [
        {
          "source": { "clipId": "suit.helmet-hold", "loop": true },
          "phase": "overlay",
          "mask": "upperBody",
          "preserveBones": [],
          "mode": "override",
          "rootPolicy": "preserve-engine",
          "claims": [
            { "target": "pose.upper-body", "access": "exclusive" },
            { "target": "attachment.hand.right", "access": "exclusive" }
          ]
        }
      ]
    },
    {
      "id": "stowed",
      "ownership": {
        "kind": "socket-clone",
        "owner": "consumer",
        "lifetime": "external",
        "socket": {
          "bone": "C_Hips",
          "translation": [0, 0, 0],
          "rotationDegrees": [0, 0, 0],
          "scale": 1
        }
      },
      "layers": []
    }
  ],
  "transitions": [
    {
      "id": "head-to-held",
      "from": "head",
      "to": "held",
      "layer": {
        "source": { "clipId": "suit.helmet-off" },
        "phase": "overlay",
        "mask": "upperBody",
        "mode": "override",
        "rootPolicy": "preserve-engine",
        "claims": [
          { "target": "pose.upper-body", "access": "exclusive" },
          { "target": "attachment.hand.right", "access": "exclusive" }
        ]
      },
      "eventFrame": 24,
      "holdFrames": 4,
      "edgeBlends": { "end": 18 }
    },
    {
      "id": "held-to-stowed",
      "from": "held",
      "to": "stowed",
      "layer": {
        "source": { "clipId": "suit.helmet-stow" },
        "phase": "overlay",
        "mask": "upperBody",
        "mode": "override",
        "rootPolicy": "preserve-engine",
        "claims": [
          { "target": "pose.upper-body", "access": "exclusive" },
          { "target": "attachment.hand.right", "access": "exclusive" }
        ]
      },
      "eventFrame": 65,
      "holdFrames": 4,
      "edgeBlends": { "start": 18 }
    }
  ]
}
```

Studio currently stores an `eventFrame`, `holdFrames`, and endpoint `edgeBlends` in authoring frames
under the route's declared `fps`. `holdFrames` holds the carrier transform after the event; it is not
a general source/destination visibility-overlap interval. A runtime compiler converts marks and
blends to documented seconds/fractions. Independent source-show/destination-hide frames are needed
if a route requires visual overlap beyond the carrier hold or consumer handoff settings.

Attachment translation, XYZ-degree rotation, and uniform scale must use the same local coordinate
contract as current `osf.prop.attach`; the final schema must state those units and conversion once,
not leave Studio and runtime consumers to infer them.

Core route events remain generic marker/complete/abort events. A Suit Protocol compiler adapter may
name or translate a marker for its handoff payload, but the actor runtime must not dispatch a
hard-coded Suit action.

Every transition clip needs the same explicit layer contract as a station layer, or a documented
inheritance rule that resolves to one. Validation maps its write mask to the claim table and
rejects uncovered writes. Equipment routes preserve the engine root by default; a clip that writes
root/world motion is rejected unless its definition explicitly acquires the exclusive `motion.root`
claim.

## Persistent conditions and profiles

A device integration normally has four independent pieces:

1. owner-scoped condition assertions while equipment is active;
2. presentation profiles and, if the candidate RE track graduates, clip-replacement rules whose
   `when` clauses match those conditions;
3. equip/remove transitions, optionally authored as routes; and
4. device visuals, which may be OSF render props or consumer-owned objects.

Condition IDs are opaque, lowercase, and namespaced to their consumer. OSF standardizes the coarse
claim targets, not every mod's device names. A device or compatibility pack connects its conditions
to profiles in data. Assertions are volatile and owner-scoped/ref-counted; clearing one owner's
assertion cannot clear another owner's matching fact. If several owners assert the same fact, its
effective enforcement is the strongest active request, while outcomes are still reported to each
owner.

Every matching profile becomes a candidate. Packs should make overlapping profiles mutually
exclusive in `when` or declare an explicit replacement/selection group. Two matching profiles that
both exclusively realize the same target without such a relation are a structured content conflict,
not an invitation for registration order to choose a winner.

A device mod should derive one aggregate desired condition set for an actor. Individual equipped
items should not each create an unaware controller that competes for the same arm or leg chains.
For example, an armbinder can supersede a weaker wrist-cuff arm pose while both device visuals and
both pieces of gameplay state remain owned by the mod.

Handcuffs mostly need an upper-body profile: the live Starfield idle/walk/run remains beneath a
full-weight arm-chain restraint pose. Ankle cuffs are different because the restriction changes
stride; the candidate RE track below tests whether engine-side clip replacement is a viable answer.

### Illustrative restraint profile

```json
{
  "schema": "draft/osf-actor-runtime/0",
  "kind": "actor-profile",
  "id": "example-restraints.front-cuffs",
  "when": { "all": ["example-restraints.hands.front"] },
  "layers": [
    {
      "id": "arms",
      "phase": "restraint",
      "claims": [{ "target": "pose.arms", "access": "exclusive" }],
      "pose": "Poses/Restraints/front-cuffs.pose",
      "mask": "bothArmChains",
      "mode": "override"
    }
  ]
}
```

The exact bones must be resolved through a skeleton-family definition. Missing required bones make
the profile unavailable for that actor; the asserted condition remains true and the owner receives
a structured realization failure. It must not degrade into partial, undefined writes.

## Candidate RE track: conditional clip replacement

OSF should investigate whether restricted locomotion can use a DAR/OAR-style native backend: when
conditions match, the engine resolves a different clip while its graph retains state selection,
timing, blending, root motion, and transitions. If proven, the compositor would simply capture the
result as part of the live base pose. The hook cadence, actor context, cache invalidation, runtime
cost, and first-/third-person graph coverage are not yet known.

The following JSON sketches desired semantics only. It is not a committed registry kind or schema;
the runtime seam must be proven before the data contract is frozen.

```json
{
  "schema": "draft/osf-actor-runtime/0",
  "kind": "clip-replacement",
  "id": "example-restraints.shackled-gait",
  "when": { "all": ["example-restraints.ankles.cuffed"] },
  "priority": 100,
  "map": [
    { "match": "actors/human/locomotion/walk*", "replace": "packs/example-restraints/locomotion/walk-shackled" },
    { "match": "actors/human/locomotion/run*", "replace": "packs/example-restraints/locomotion/run-shackled" }
  ]
}
```

Candidate semantics, contingent on the proven backend:

- Rule sets are registry data keyed on conditions; there is no per-actor replacement API. Asserting
  the condition is the activation.
- Higher `priority` wins among matching rule sets; ties are a structured content conflict.
- A replacement clip must match the original's rig fingerprint and hold the same duration/root-
  motion contract the surrounding graph assumes, or the rule is rejected at validation.
- Activation timing follows whatever binding/cache boundary the RE spike proves. The track must
  characterize whether an already-playing clip can change and how a consumer requests a safe state
  refresh; this RFC does not promise next-bind behavior.
- The consumer still owns any gameplay speed limit. Replacement changes what the gait looks like,
  never how fast the actor actually moves.

The prerequisite is RE work: locate a safe point that has both the resolved animation asset and the
actor/graph-instance context needed for per-actor conditions, then prove its threading, lifetime,
caching, and restoration behavior in game. Until that succeeds, OSF commits to no replacement API,
registry kind, wildcard syntax, or performance claim. If the runtime hook proves infeasible, offline
behavior-graph patching with an OSF-toggled variable is a separate fallback investigation; it may
need graph-specific data rather than the mapping sketch above.

The initial proof target is idle plus basic directional walk/run on two actors with different active
conditions. Starts, stops, slopes, jumps, weapons, and other engine postures are out of scope until
the seam and basic isolation are demonstrated.

## Claims and arbitration

Three pieces of metadata answer different questions:

- a condition says what persistent fact the consumer asserted;
- a claim says which coarse semantic target a layer controls and whether that access is exclusive
  or composable; and
- a bone mask says which exact transforms the compositor writes.

The claim list is flat, closed, and small:

| Target | Overlaps |
| --- | --- |
| `pose.body` | `pose.upper-body`, `pose.arms`, `pose.lower-body` |
| `pose.upper-body` | `pose.body`, `pose.arms` |
| `pose.arms` | `pose.body`, `pose.upper-body` |
| `pose.lower-body` | `pose.body` |
| `pose.face` | (reserved for future face layers) |
| `motion.root` | — |
| `attachment.hand.left` | — |
| `attachment.hand.right` | — |
| `attachment.hip` | — |

Every target overlaps itself. There is no hierarchy expansion, no ancestor closure, and no separate
capability vocabulary: "are the hands free" is answered by querying whether anything exclusively
claims `pose.arms` or the hand attachment targets. If real content later demands finer targets
(per-arm, stride), they can be added with their overlap rows without breaking existing data; the
Skyrim device ecosystem ran for a decade on biped slots and keywords, so the burden of proof is on
the finer target.

`exclusive` access conflicts with any overlapping writer unless the scene explicitly `covers` the
claiming condition. `composable` access is allowed only for declared phases/orders and compatible
pose modes. Validation also verifies that every weighted bone in a mask is covered by the layer's
pose claim; an arms claim cannot conceal an `upperBody` write.

For each state change or scene start, arbitration should:

1. collect routes, asserted conditions, resolved profiles, gestures, and scene entries;
2. compare claims, requirements, scene compatibility, and consumer enforcement;
3. select an authored variant where available;
4. accept, suspend, or reject each entry deterministically;
5. construct the layer plan; and
6. emit a reasoned outcome to affected owners.

Enforcement has two tiers:

| Enforcement | Meaning |
| --- | --- |
| required | Do not hide or weaken this condition's realization. Reject an incompatible scene/request. |
| cosmetic | May suspend or fade when its claimed resource is needed elsewhere; reconciled afterward. |

The consumer chooses enforcement. A restraint mod may mark locked handcuffs required. Suit Protocol
marks a held-helmet gesture cosmetic, allowing a cinematic scene to suspend it while the semantic
helmet state remains intact.

Enforcement does not grant OSF gameplay preemption authority. If a required assertion arrives while
an incompatible scene is already committed, OSF retains the assertion, reports its realization as
conflicted/pending, and does not silently stop the scene. The consumer may stop the scene, queue the
realization, or apply an instant gameplay-specific fallback. The same assertion present during a new
scene preflight rejects that start under strict policy. This is intentional **committed-holder
precedence**: a consumer that needs required mid-scene preemption must explicitly stop/replace the
scene.

Suspension retains desired state but releases pose and prop resources according to their lifetime.
When the conflict ends, the controller re-runs arbitration and reconciles from current semantic
truth. It does not blindly resume a clip at an old time.

## Scene integration

`SceneRuntime` remains the only owner of a live scene handle. Its actor exclusivity rule still means
one cinematic scene per actor. The change is that a scene no longer owns the actor's only possible
OSF sampler; it submits one synchronized scene body layer to that actor's controller.

Existing responsibilities remain unchanged:

- scene definition and node graph validation;
- role binding and matchmaking;
- shared clocks, stage timing, and navigation;
- anchors, participant placement, and root pinning;
- timed action, camera, sound, and cue lanes;
- control, equipment, fade, and camera policy mechanisms; and
- the per-scene undo ledger and public callbacks.

Sound remains a one-shot timed lane event, not a reversible ledger mutation.

At scene start, the runtime obtains an arbitration result for every role before committing any
participant. If one required actor condition is incompatible, start fails transactionally rather
than partly starting the roster. The group transaction is:

1. pin definitions and reserve every actor/claim in stable actor/resource order;
2. validate clips, skeleton compatibility, policies, and external prerequisites;
3. apply reversible strip/equip/visibility/placement mutations under the group ledger;
4. re-resolve actor model roots, rigs, masks, and attachments after any topology-changing equipment
   work;
5. prepare all role clip instances and pose plans; then
6. commit the handle/group and permit public callbacks.

Any failure unwinds the ledger and reservations in reverse order. Mutations are therefore
compensatable, not forbidden during preparation, and no partially started roster becomes public.

At each node/stage change, the scene adapter replaces the role's clip source while preserving the
outer scene handle, ledger, and synchronization identity. A scene either proves one compatibility
contract for every reachable node or preflights each node transition as a smaller group
transaction. A failed navigation remains at the current node or ends the scene according to
explicit scene policy; it never advances only part of the roster. A changed condition also triggers
this preflight before the next node.

Scene end ordering is an observable invariant:

1. stop/release scene body playback and claims;
2. unwind the existing scene ledger;
3. synchronously dispatch `SCENE_END` while actor-profile resume is gated;
4. retire the scene handle; and
5. re-arbitrate actor controllers, then dispatch profile/route resumed outcomes.

This lets an end callback change consumer truth before one final reconcile. World-load teardown
suppresses public callbacks, clears scene and actor-runtime state, and relies on consumers to
reassert after load.

Legacy scenes require no new metadata to play exactly as they do today when no actor conditions are
registered. For the compatibility adapter, a legacy role with no claims is treated as today's
exclusive full-body scene posture. Cosmetic profile realizations suspend while it plays, but their
conditions remain asserted. A consumer that marks a realization required must explicitly choose
whether unknown legacy scenes are rejected or allowed to use the generic masked fallback; OSF must
not make that policy choice invisibly, and it reports that the match was unknown rather than
silently claim it was compatible.

Equipment stripping is a separate preflight. A required restraint can expose protected forms,
slots, or keywords that the scene's strip plan must retain. A scene that truly requires their
removal is incompatible; merely accepting the restraint's bone pose is not enough. Existing
`stripActors` behavior remains unchanged for actors with no protected equipment claim.

## Restraints over a scene

There are two quality tiers.

| Tier | Technique | Appropriate use | Limitation |
| --- | --- | --- | --- |
| 1 | Full-weight masked pose over the scene | Front cuffs in many standing/kneeling poses; simple upright armbinders | Fixed arms can intersect the body, partner, furniture, or floor |
| 2 | Fully authored restrained variant | Load-bearing, close-contact, furniture-bound, or silhouette-critical scenes | Requires additional content |

This mirrors what actually shipped in a decade of Skyrim device content: full-weight bound poses
plus an animation filter that swapped incompatible scenes for authored bound variants. A tier-1
restraint must drive complete limb chains, not just wrists — blending a cuff pose at 50 percent
does not keep the hands together; use blends only while its realization enters or exits.

Tier 2 remains necessary when the original animation assumes:

- an arm bears weight or prevents a fall;
- a hand contacts a partner, prop, wall, or furniture;
- behind-the-back arms would intersect the floor or seat;
- an armbinder materially changes the torso silhouette; or
- ankle restraints invalidate the base hip width, balance, or foot placement.

The matchmaker should prefer, in order:

1. an exact authored variant for the active condition set;
2. a scene explicitly allowing the generic masked fallback (`covers` or compatible claims); and
3. rejection or an explicit consumer-selected degradation policy.

This avoids both extremes: authoring every possible scene/device combination and pretending a bone
mask can solve every physical interaction. A runtime IK middle tier is deliberately deferred (see
appendix); if authored variants plus the masked fallback prove insufficient in practice, that is
the evidence that would justify building it.

### Illustrative scene compatibility metadata

```json
{
  "schema": "draft/osf-actor-runtime/0",
  "roles": [
    {
      "name": "receiver",
      "presentation": {
        "requirements": {
          "conditions": { "none": ["example-restraints.arms.back"] }
        },
        "claims": [
          { "target": "pose.body", "access": "exclusive" }
        ],
        "covers": ["example-restraints.hands.front"]
      }
    }
  ]
}
```

This shape is illustrative. `requirements` decides whether the actor may fill the role. `claims`
describes what the playback controls. `covers` means the authored clip already depicts a named
condition, so that condition's overlapping persistent pose layers yield while the role plays. The
assertion and protected device visuals remain active unless their own ownership contract explicitly
transfers them.

For example, a custom front-cuffed variant would require `example-restraints.hands.front`, list
that same condition in `covers`, and claim its authored arm pose exclusively. Compatibility is
role-local, survives node changes only where declared valid, and is queryable by matchmaking before
the scene starts.

## Props and ownership transfer

Animation state and prop lifetime are related but must not be conflated.

Proposed prop lifetimes are:

| Lifetime | Example | Cleanup owner |
| --- | --- | --- |
| transition | Helmet clone visible only during an equip handoff | Route transition |
| station | Helmet carried while Held | Route station |
| profile | Temporary cuff visuals selected while a condition is asserted | Profile lease/owner |
| scene | A temporary scene prop | Existing scene ledger |
| controller | A render helper shared by several states | Actor controller |
| external | Suit Protocol's maintained hip clone or a real equipped device | Consumer mod |

Studio's current station ownership kinds already express the helmet case:

- `equipped-armor`: gameplay equipment owned by Suit Protocol/Starfield;
- `carrier`: an OSF render clone attached to `R_AnimObject1`; and
- `socket-clone`: a durable clone at a consumer-maintained socket such as `C_Hips`.

The draft runtime schema makes `owner` and `lifetime` explicit even when today's Studio kind implies
them, so cleanup never infers that a consumer-owned socket clone belongs to OSF.

An ownership marker produces an ordered, idempotent handoff — not a transaction. Each transfer
carries a generation; every handoff step is idempotent for that generation, and the ordering
guarantee is that the destination is created/revealed before the source is asked to hide/release,
so the object never visibly vanishes mid-transfer. Callbacks to external owners are synchronous on
the game thread and return an acknowledgement/failure result. On a failure partway through, OSF
removes any OSF-owned prop it prepared, reports a structured route failure naming the last
completed step, and leaves the consumer's own objects alone. There is no rollback protocol for
external mutations: the consumer's load/reassert reconciliation — which it must implement anyway —
is the universal repair path, and reasserting the current desired station after a failed handoff
must converge to a correct visual.

Suit Protocol should continue owning its persistent hip clone unless OSF gains all of the required
maintenance guarantees. It already knows helmet identity, save reconciliation, actor-root
replacement, and its measured no-topology-change visibility strategy. The generalized runtime does
not need to absorb that domain-specific ownership to animate the route coherently.

An `external` lifetime is reference and handoff metadata only. OSF neither destroys nor repairs the
consumer's object when the route/controller ends.

A rigid prop cannot have two skeleton parents. Wrist cuffs should normally be two wrist-attached
pieces plus a connector rendered from both endpoints (or a separately simulated chain), rather than
one object alternately attached to one wrist.

## Consumer API shape

The eventual native ABI should follow OSF's existing versioned, size-prefixed C-table conventions.
The operations below describe semantics, not final C signatures:

```text
AcquireOwner(pluginId, callbacks) -> owner
CreateRoute(owner, actor, routeId, initialStation) -> routeInstance
RequestStation(routeInstance, destination, requestToken) -> accepted/pending/rejected
AssertCondition(owner, actor, conditionId, enforcement, requestToken) -> assertion
AssertConditions(owner, batch) -> assertions          (bulk; one arbitration pass)
ClearCondition(assertion, requestToken)
QueryActorPresentation(actor) -> accepted/suspended/failed entries + scene compatibility
ReleaseRoute(routeInstance)
ReleaseOwner(owner)
```

`AssertConditions` exists specifically for load-time reconstruction: a consumer restoring a dozen
device conditions after load must be able to do it in one game-thread call with one arbitration
pass, not a dozen round-trips through the SFSE task queue.

Callbacks/events should include:

```text
owner, actor, instance, requestToken, eventType,
route/station/transition, condition, or resolved profile id,
authored marker id, outcome/reason, current scene handle (if relevant)
```

Callbacks used for an external ownership endpoint must be synchronous on the game thread, as
current native scene cues are. Informational Papyrus events may remain asynchronous. The caller
token is echoed for correlation; OSF's generation controls stale suppression. Reentrant calls are
queued and applied after the current dispatch boundary, while start-time events are buffered until
the returned handle is bound, matching the safety property already needed by Suit Protocol.

`ReleaseOwner` is a callback/deferred-command barrier: after it returns, no later dispatch may
reference that owner. A release called reentrantly marks the owner closing immediately, suppresses
further events, and completes logical cleanup after the current callback unwinds. Retired immutable
pose data may outlive the owner context until the compose-reader reclamation invariant permits
freeing it.

No API should expose a raw `ClipInstance`, pose buffer, or internal node pointer to consumers.

## Authoring flow

Studio should be the source of truth for route geometry and transition timing.

For routes, an author:

1. defines stations and their ownership;
2. captures station body poses and attachment sockets;
3. links or creates one transition clip per route edge;
4. aligns the clip endpoints to the station poses;
5. places ownership and optional cue markers;
6. previews edge blends, carrier holds, and explicit handoff overlap; and
7. exports a runtime route plus any consumer handoff contract/config generated from the same source.

The existing Head/Held/Stowed route already contains most of this information. Its next step is a
generic compiler target, not another set of hand-maintained scene IDs.

Today Studio can render route-derived OSF actions and Suit handoff output, but it cannot emit or load
the proposed actor-route/profile runtime schema. The draft blocks in this RFC are requirements and
examples for that future compiler, not files that current OSF will accept.

For profiles, Studio needs a fallback pose and mask previewed over arbitrary base clips, a `when`
condition selector, and claim-target validation. Validation should distinguish an unknown
namespaced condition (valid but supplied by an optional consumer) from an unknown claim target (an
interoperability error).

For authored variants, Studio should let an author preview the base scene with active conditions,
then save only the replacement role clip or sparse correction where possible. The registry and
matchmaker need the compatibility metadata regardless.

If the candidate clip-replacement track graduates, its authoring flow will be designed around the
proven seam. The mapping sketch suggests rule data plus replacement clips, but its validation and
tooling requirements are deliberately not frozen yet.

## End-to-end examples

### Suit Protocol: timed or indefinite held helmet

1. Suit Protocol decides that the desired semantic state is Unsealed and requests Head -> Held.
2. The route transition plays as an upper-body overlay over live locomotion.
3. At the ownership marker, OSF creates/attaches the carried helmet and emits the synchronous
   generation-stamped handoff event. Suit Protocol updates its render-only visibility transaction
   and may commit Unsealed semantic state before the presentation transition fully finishes.
4. Held becomes the committed station. Its station pose and carried prop can remain active for any
   duration without occupying the cinematic scene slot.
5. In timed mode, Suit Protocol later requests Stowed. In explicit mode, it leaves Held desired.
6. During Held -> Stowed, the authored marker reveals Suit Protocol's persistent `C_Hips` clone at
   the matching handoff transform before the OSF carrier is destroyed.
7. Stowed has no pose layer. OSF stops sampling while Suit Protocol maintains the durable clone.
8. A reverse request follows Stowed -> Held -> Head, using the same ownership rules.

If a cinematic scene starts during this cosmetic gesture, the controller reports suspension or a
safe interrupted station. Suit Protocol retains Unsealed as semantic truth and requests the correct
presentation again when the scene ends.

The ordinary stable mapping is Sealed -> Head and Unsealed -> Stowed. Held remains a presentation
station unless Suit Protocol explicitly persists a separate latched-hold preference; even then it
saves the preference, never the clip time.

### Handcuffs during ordinary locomotion

1. The device mod equips cuffs and persists that gameplay state.
2. It asserts required `example-restraints.hands.front`; the registry selects the matching
   front-cuff profile, while the mod owns or leases the cuff visuals.
3. Starfield continues to provide idle, walk, and run poses.
4. The restraint phase overwrites the complete arm chains after the live base pose.
5. On removal, the mod runs any remove route, clears its condition assertion, and OSF blends the
   profile realization to the live underlying arms.

No custom walk and idle clips are required if the lower body and torso remain plausible.

### Handcuffs during an intimate multi-actor scene

1. The matchmaker queries each actor controller and sees a required front-cuff condition.
2. It selects an authored cuffed variant, or a scene whose role `covers` the condition or leaves
   the arm claims uncontested for the generic masked fallback.
3. `SceneRuntime` submits the synchronized scene body clip as the actor's base scene layer.
4. The restraint layer overrides the arm chains after that base.
5. Cuff pieces follow the final wrist transforms; a connector follows both endpoints.
6. When the scene ends, the scene body layer disappears. The still-asserted condition reselects its
   profile over Starfield locomotion.

If the scene uses the actor's arms for support, the generic fallback is rejected and an authored
restrained variant is required. No blend weight can repair the missing support assumption.

### Ankle cuffs: candidate native-backend outcome

If the clip-replacement RE track graduates:

1. The device mod asserts its ankle condition; a matching replacement rule set becomes eligible.
2. In ordinary play, the engine plays shackled idle/walk/run replacements while retaining ownership
   of movement and world translation; OSF's compositor is not involved.
3. A cinematic scene that authors a sufficiently narrow lower-body pose may accept the condition
   without any replacement mattering because scenes bypass locomotion.
4. A wide stance or load-bearing foot placement still requires a compatible variant or rejection.

## Interruption, failure, and reconciliation

The runtime must make failure observable and leave semantic ownership unambiguous.

- **Missing clip or invalid rig:** reject before committing the destination and report a structured
  reason. The consumer chooses an instant visual fallback or retains its previous presentation.
- **Countermand:** advance OSF's transition generation, follow the authored reversal policy, and
  suppress stale work while echoing the consumer's token only as correlation data.
- **Cinematic conflict:** required condition realizations reject incompatible starts; cosmetic
  entries suspend and later reconcile.
- **Actor 3D unavailable:** retain desired state without sampling/stamping. Rebind and reconstruct
  when a valid skeleton returns.
- **Owner release/plugin unload:** remove only that owner's entries and clean its OSF-owned props.
- **Scene abort:** release all scene body layers through the existing ledger end path, then reconcile
  controllers.
- **World-replacing load:** clear every runtime instance and raw actor reference. Consumers rebuild
  from persisted semantic state (see `AssertConditions`); in-flight transitions are never restored.
- **Death, furniture, combat, or menus:** OSF exposes presentation availability and reasons; each
  consumer decides whether its request is required, deferred, instant, or cancelled.

Reached-station and ownership-marker callbacks must be idempotent. A consumer should be able to
reassert its current desired station/condition at any time without duplicating a prop or replaying a
completed transition; this reassert-to-converge property is also the repair path after any failed
handoff.

## Delivery plan

Phases 1–3 are sequential. The candidate clip-replacement RE track is independent and is not a
dependency of the actor runtime. No product or schema work for it lands before the runtime seam is
proven.

### Phase 0: formalize assets and contracts

- Keep the current Suit Protocol implementation working.
- Specify route runtime data from Studio's existing station/transition model.
- Define the claim table, skeleton-family masks, request outcomes, and compatibility metadata.
- Add registry/parser fixtures before shipping authored graph/route content.
- Add a compatibility adapter that can compile the Studio route to today's scenes and typed
  callbacks, allowing current assets and the new route source to be parity-tested side by side.

### Phase 1: one persistent actor overlay controller

- Add owner-scoped route instances and zero-animation stations.
- Add desired-station requests, typed lifecycle events, reversal, and reconciliation.
- Add standalone mask/mode/weight/`preserveBones` policy, timed route markers, and route-owned prop
  leases; current standalone playback is not sufficient by itself.
- Support transition-, station-, and external-prop reference/handoff lifetimes.
- Allow one non-cinematic overlay at a time and suspend it around current scenes.
- Fade/remove that overlay safely before admitting a cinematic scene, and add transition-progress
  stall/3D-loss handling.
- Migrate Suit Protocol from several hard-coded scene IDs to one generated route contract.

This phase solves cohesive helmet behavior and indefinite Held without requiring the final
multi-layer compositor.

The initial new-controller scope is the human full-body/third-person actor skeleton used by the
helmet route. It does not promise simultaneous first-person arm rigs, alternate LOD skeletons, or
new creature profiles. Existing scene playback for other supported species remains on its legacy
path until a compatible actor binding is defined. Missing/replaced model roots are reported as a
suspended or failed realization, never treated as silent actor-wide coverage.

### Phase 2: one-stamp multi-layer compositor

- Separate clip sampling from final rig stamping.
- Capture the live engine base once and compose multiple accepted masks/modes.
- Lower existing scene role playback through `ScenePoseAdapter`.
- Add owner claims, enforcement, arbitration, and deterministic suspend/resume.
- Preserve existing scene behavior when no additional layers are active.

This phase makes conditions, profiles, and generic masked restraint poses possible — including over
compatible scenes.

### Phase 3: scene compatibility and variant matchmaking

- Add role `requirements`/`claims`/`covers` metadata and matchmaker compatibility queries.
- Add authored-variant preference and structured rejection/degradation outcomes.
- Add equipment-strip protection preflight.
- Add Studio preview of profiles/variants over scene clips.

Together with Phase 2 this covers a Devious-Devices-class integration's launch needs: persistent
restraint presentation plus scene filtering/variant selection — the combination that carried the
Skyrim device ecosystem.

### Candidate RE track: conditional clip replacement

- Pin an actor-aware substitution seam and characterize threading, caching, invalidation, and
  first-/third-person graph coverage.
- Prove condition-isolated replacement on two actors, followed by clean deactivation, graph rebuild,
  save/load, and world-replacement behavior.
- Only after that proof, decide whether a registry kind, priority rules, wildcard syntax, and
  rig/duration validation are appropriate.
- If the hook is infeasible, separately evaluate offline behavior-graph patching with an OSF-toggled
  variable; do not assume it can use the same mapping data.

Anything beyond this — finer claim targets, additional gesture channels, face layers, runtime IK —
is pulled by demonstrated content needs, not scheduled. See the appendix.

## Compatibility and migration

- Existing `*.osf.json` scenes remain valid and retain current defaults.
- Existing scene native/Papyrus handles remain scene handles; actor-route handles use a separate API
  and namespace.
- Existing role mask/`preserveBones`/mode/weight settings lower into a scene body layer without
  semantic change.
- Current scene graph nodes and timed lanes remain the orchestration source during migration.
- Suit Protocol can migrate transition by transition. Its transactional core, persistent hip clone,
  and load reconciliation remain authoritative.
- A device mod can first use a generic persistent upper-body pose, then adopt variants; conditional
  clip replacement remains optional unless its RE track graduates.

Internally, `GraphManager` can initially adapt the old one-graph path into the compositor. A large
rename is not a prerequisite. The invariant to establish early is that only the compositor writes
the final actor pose.

## Performance and safety constraints

- Capture and convert the live actor pose once per actor per frame.
- Sample only active layers; cache static station/restraint poses.
- Stamp the flat rig buffers once after all composition.
- Bound active layers; log rejected overflow rather than degrade unpredictably.
- Keep arbitration event-driven. Only active clip sampling belongs in the frame path. Any candidate
  clip-replacement hook must measure its cadence and avoid per-frame registry or pose work.
- Do not add per-frame scene registry lookups, form resolution, prop cloning, or skeleton topology
  mutation.
- Retain generational handles and never keep raw actor/skeleton references across world replacement.
- Validate every deferred command against both its handle generation and the current world epoch.
- Keep same-tick event order deterministic and documented.
- Report composition/arbitration diagnostics below the default user log level unless an operation
  fails.

## Validation criteria

The architecture is successful when all of these are true:

1. A player can walk indefinitely in Held with a carried helmet, then stow or re-equip it through
   the same route.
2. A zero-animation Head or Stowed station does not block an unrelated OSF scene.
3. Front cuffs can override arm chains over Starfield locomotion without replacing the legs.
4. A compatible multi-actor scene and a required cuff restraint compose in one deterministic final
   pose.
5. An arm-supported incompatible scene is rejected or selects an authored variant rather than
   silently breaking the pose.
6. Every ownership transfer shows exactly the intended visual through completion, countermand,
   abort, actor 3D rebuild, and load reconciliation.
7. With no actor routes, assertions, or profiles active, legacy scene output and timing are
   unchanged.
8. Layer registration order cannot change the final pose.
9. Tests cover arbitration, masks, composition order, transition markers, stale request tokens,
   suspension/reconciliation, cleanup, and transactional multi-role scene start.

The candidate clip-replacement track has separate graduation criteria: condition-isolated swaps on
two actors, correct first-/third-person behavior, clean restoration across graph and world rebuilds,
and measured hook cadence without cross-actor leakage.

## Open design decisions

The architecture does not depend on these spellings, but implementation must settle them before the
new schema/API is public:

- whether route/profile assets live in the unified scene file or separate registries;
- the exact compatibility policy for legacy scenes when a consumer does not specify one;
- whether route reversal can scrub a transition backward or always follows authored reverse clips;
  and
- how two-parent connectors such as chains are rendered and culled.

The defaults recommended by this RFC are conservative: explicit reverse clips, consumer-selected
legacy policy, one cinematic scene per actor, required restraints never silently suspended, and one
final pose stamp.

## Appendix: deferred and sketched

Cut from the committed design, recorded so later additions extend rather than contradict it. Each
item ships only when real content demonstrates the need.

- **Runtime IK / procedural constraints.** The first draft specified a middle restraint tier:
  paired-effector limb solving against scene-authored constraint targets (`pairedWrists.front`
  published per role, pole vectors, joint limits, reach diagnostics), composing as a distinct
  post-local model-space solve. Deferred because the equivalent tier effectively never existed in
  the Skyrim device ecosystem and content did fine on masked poses plus authored variants. If
  revived: it slots in as a new layer source in the restraint phase, scene roles gain a
  `supports`/`constraintTargets` block, and nothing in the current claim table changes.
- **Finer claim targets.** Per-arm/per-leg pose targets, `motion.stride`, and explicit contact
  resources (`support.arm.left`), plus hierarchical expansion. Add rows to the flat table with
  explicit overlap entries instead; hierarchy only if the table grows past what a human can audit.
- **Capability vocabulary.** `hands.free`-style derived facts. Currently answered by claim queries;
  a first-class vocabulary returns only if consumers demonstrably need facts that claims cannot
  express.
- **`preferred` enforcement tier.** "Keep when compatible, suspend under an exclusive scene, then
  reconcile" — dropped because it was nearly indistinguishable from `cosmetic`. Revive only with a
  concrete case where the two must diverge.
- **Locomotion blending profiles.** Movement-parameter-driven in-place clip selection blended by
  OSF (grounded state, planar speed, direction, turning). Deferred while the candidate native
  replacement track is investigated; reconsider only if no safe native backend proves sufficient
  for demonstrated restricted-movement content.
- **Face/expression layers.** A `pose.face` layer source for gags, blindfolds, and expressions,
  with morph-set references in profiles. The claim target is reserved; the design is not started.
- **Multiple simultaneous gesture channels.** More than one soft additive/gesture layer per actor;
  consider after two real integrations need them.
- **Transactional external prop handoff.** The first draft specified a
  prepare/acknowledge/commit/compensate protocol with an at-most-once marker journal. Replaced by
  the ordered idempotent handoff plus reassert-to-converge repair; revive only if a consumer
  appears whose external mutations genuinely cannot be repaired by load-style reconciliation.
