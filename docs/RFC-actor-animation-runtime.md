# Actor animation composition and state routing

Status: proposed. This document describes a target architecture; none of the new runtime, API, or
schema names below should be read as implemented behavior.

Related current contracts: [scene schema](SCENE_SCHEMA.md), [consumer API overview](API.md), and the
authoritative [native scene ABI header](../src/API/OSFSceneAPI.h).

## Decision

OSF should add a per-actor animation runtime beneath `SceneRuntime`, rather than turn every durable
actor condition into a long-running scene.

The runtime accepts several kinds of presentation intent for an actor:

- a short **route** between presentation stations, such as helmet Head -> Held -> Stowed;
- a persistent **condition assertion**, matched to a presentation profile such as front handcuffs or
  an armbinder;
- an optional **locomotion profile**, such as an ankle-cuffed idle/walk/run set;
- the actor's role in one OSF **scene**; and
- small compatible gestures or additive layers.

It arbitrates those intents, samples every accepted layer against one captured Starfield pose, and
stamps one final pose. Consumer mods continue to own gameplay truth and policy. OSF owns the
temporary animation state needed to present that truth.

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
7. Support a quality ladder for restraints: generic masked pose, procedural constraint, and fully
   authored scene variant.
8. Keep existing scene definitions and APIs working while the lower runtime changes incrementally.
9. Reuse the route model already present in Studio instead of creating a second helmet-specific
   authoring format.

## Non-goals

- OSF does not decide whether a helmet should be sealed, whether a device is equipped, or whether a
  restrained actor is allowed to enter a scene.
- OSF does not persist consumer gameplay state. It may cache presentation state only for the loaded
  world.
- OSF does not change actor movement speed, inventory, AI packages, combat rules, or escape logic.
- This is not a replacement for Starfield's complete locomotion graph.
- Bone blending and IK cannot automatically make a scene physically plausible when the original
  pose depends on a restrained limb for balance or contact.
- The first implementation does not need arbitrary cross-actor physics, cloth simulation, or an
  unbounded number of layers.

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
  pose/constraint layers, capabilities, claims, props, and optionally a locomotion profile. It is
  not a scene and does not own gameplay truth.
- **Layer:** One contribution to an actor pose: a clip, cached pose, additive motion, or procedural
  constraint. A layer declares where it composes, which bones it writes, and which semantic
  resources it claims.
- **Locomotion profile:** A movement-driven group of in-place clips or poses. It maps generic
  movement parameters to an appropriate visual rather than duplicating gameplay movement policy.
- **Claim:** A semantic resource needed for an animation to make sense, such as both hands, the left
  arm as a support limb, or lower-body stride. Claims prevent combinations that a bone mask alone
  cannot diagnose.
- **Constraint target:** A scene- or actor-relative target used by a procedural solver, for example
  a paired-wrist target in front of the pelvis.
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
requests.

## Target architecture

```text
  Suit Protocol       restraint mod       other consumer
  semantic state      equipped devices    semantic state
        |                    |                   |
        +------- desired stations / conditions / profiles -------+
                                                                 |
  Matchmaker + SceneRuntime ---------------- scene-role intent ---+
                                                                 v
                ActorAnimationService (one controller per actor)
                +-----------------------------------------------+
                | route instances and desired stations          |
                | condition assertions and resolved profiles   |
                | semantic claim arbitration                    |
                | suspension, interruption, and reconciliation  |
                +----------------------+------------------------+
                                       |
                                  accepted layers
                                       v
                              PoseCompositor
                live Starfield pose -> base -> overlays -> constraints
                                       |
                                  one final stamp
                                       |
                                  actor skeleton

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
|- zero or one locomotion profile
|- zero or more route instances
|- zero or more owner-scoped condition assertions
|- zero or more resolved presentation profiles
|- zero or more accepted gesture/additive layers
|- transition/state/controller-scoped prop leases
`- desired, active, suspended, and failed request records by owner
```

Every request has an owner identity. Releasing or unloading one owner removes only that owner's
routes, conditions, profiles, and props. A caller-supplied request token is correlation data only;
OSF assigns its own monotonic instance and transition generations for stale-work rejection. An
at-most-once event key includes the world epoch, instance generation, transition generation, and
marker index.

An instance pins immutable route/profile, mask, skeleton, and claim-registry snapshots for its
lifetime. Reloading packs affects new instances or an explicit safe reconcile, never the meaning of
an in-flight edge or its cleanup plan.

A stable station can contain zero layers. When every accepted entry is zero-layer, the controller
has no sampling cost and does not occupy the actor's cinematic scene slot.

### Layer contract

A resolved layer needs at least:

| Field | Purpose |
| --- | --- |
| owner and instance | Isolated cleanup and callback routing |
| source | Clip, static pose, additive pose, or solver |
| phase | Base, overlay, or hard constraint |
| write mask | Bones and feather weights that the compositor actually changes |
| pose mode and weight | Override or rest-relative additive composition |
| blend policy | Enter, exit, transition, and interruption blends |
| claims | Semantic resources required or made unavailable |
| enforcement | Required, preferred, or cosmetic intent supplied by the consumer |
| lifetime | Transition, station, profile, controller, scene, or external |
| order | Deterministic visual ordering among compatible layers in the same phase/target |

Layer order is not request urgency, scene matchmaking priority, or a substitute for compatibility.
If a base scene uses both arms to support the actor, silently sorting a handcuff layer later produces
a broken pose. That combination must instead select a compatible variant, use an authored
constraint target, or be rejected under the consumer's policy.

Pose layers are only one actor-local resource domain. Root/world placement, movement/AI control,
equipment visibility or protection, and attachment ownership also require explicit claims. Root
placement is exclusive and is not blended as a local-pose layer. Scene-global camera, fade, sound,
and player-control mechanisms remain in `SceneRuntime` and its ledger.

## Pose composition

The compositor runs after Starfield has evaluated the actor and before the final rig buffers are
consumed, matching the current hook placement.

Controller mutations, arbitration, props, and public callbacks are game-thread work. The game
thread publishes an immutable pose plan for animation hooks to consume. A hook copies live rig
locals into compose-owned per-frame scratch before any OSF write; it never retains a borrowed rig
buffer pointer in controller state and never rereads a buffer containing OSF's prior stamp. Marker
crossings discovered off the game thread are sequence-stamped and queued for ordered game-thread
dispatch.

At the controller command barrier, completed-frame marker events drain in frame/track order before
new API commands are applied and the next immutable plan is published. Every event names the plan
and transition generation that crossed it. A result from an older plan after a countermand is
dropped; a checkpoint that genuinely crossed before the countermand is journaled and delivered
before the newer request is planned.

Published plans use shared ownership or epoch/RCU-style reclamation. Animation/compose threads pin
the exact plan, clip, skeleton binding, masks, and immutable definitions they read. Owner release
publishes a tombstone/new plan first; physical reclamation and destruction of related OSF props wait
until every reader of the retired epoch is quiescent. World clear stops publication, establishes the
same quiescence fence, then frees raw actor/model references.

For each actor and frame it should:

1. Capture the live Starfield local pose once. This remains the immutable base for the frame.
2. Evaluate the accepted **base** source:
   - ordinary play keeps the live Starfield pose;
   - a locomotion profile may replace only its declared movement bones; and
   - a cinematic scene body layer normally supersedes a locomotion profile on the bones it owns.
3. Apply compatible **overlay** layers, including equipment gestures, upper-body poses, and additive
   motion. Masks and feathered seams determine the numerical blend.
4. Apply accepted **hard constraints** after the scene and soft overlays. A hard restraint uses a
   full-weight chain pose or a solver; partial weighting is only for entering and leaving it.
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

Procedural constraints are a distinct post-local step. They build the fully composed model-space
chain parent-first, solve effectors/joint limits there, and convert the solved transforms back to
locals against their final parents. They are not ordinary cached local-pose clips.

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
- the most recent irreversible ownership/gameplay checkpoint;
- an at-most-once journal of markers already delivered for the transition generation; and
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
        "preserveBones": [],
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
        "preserveBones": [],
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
inheritance rule that resolves to one. Validation maps its write mask to the claim hierarchy and
rejects uncovered writes. Equipment routes preserve the engine root by default; a clip that writes
root/world motion is rejected unless its definition explicitly acquires the corresponding exclusive
root/motion claim.

## Persistent conditions and locomotion

A device integration normally has four independent pieces:

1. owner-scoped condition assertions while equipment is active;
2. presentation profiles whose `when` clauses match those conditions;
3. equip/remove transitions, optionally authored as routes; and
4. device visuals, which may be OSF render props or consumer-owned objects.

Condition IDs are opaque, lowercase, and namespaced to their consumer. OSF standardizes coarse
capabilities, constraint targets, and claim targets, not every mod's device names. A device or
compatibility pack connects its conditions to profiles in data. Assertions are volatile and
owner-scoped/ref-counted; clearing one owner's assertion cannot clear another owner's matching fact.
If several owners assert the same fact, its effective enforcement is the strongest active request,
while outcomes are still reported to each owner.

Every matching profile becomes a candidate. Packs should make overlapping profiles mutually
exclusive in `when` or declare an explicit replacement/selection group. Two matching profiles that
both exclusively realize the same target without such a relation are a structured content conflict,
not an invitation for registration order to choose a winner.

A device mod should derive one aggregate desired constraint set for an actor. Individual equipped
items should not each create an unaware controller that competes for the same arm or leg chains.
For example, an armbinder can supersede a weaker wrist-cuff arm pose while both device visuals and
both pieces of gameplay state remain owned by the mod.

Handcuffs mostly need an upper-body profile. The live Starfield idle/walk/run can remain beneath a
full-weight arm-chain constraint. Ankle cuffs are different: the restriction changes stride, so
their profile may include both a paired-ankle constraint and a locomotion profile.

A locomotion profile consumes generic observed parameters such as grounded state, planar speed,
movement direction, and turning. It selects and blends in-place visuals while Starfield continues to
move the actor through the world. The consumer remains responsible for any gameplay speed limit.

OSF should begin with a small explicit profile contract rather than clone Starfield's behavior
graph. Idle and basic directional walk/run coverage is sufficient for the first version; starts,
stops, slopes, jumps, weapons, and every engine posture can remain on the live base until separately
supported.

### Illustrative restraint profile

```json
{
  "schema": "draft/osf-actor-runtime/0",
  "kind": "actor-profile",
  "id": "example-restraints.front-cuffs",
  "when": { "all": ["example-restraints.hands.front"] },
  "capabilities": { "suppress": ["hands.free", "hands.hold"] },
  "layers": [
    {
      "id": "arms",
      "phase": "constraint",
      "claims": [{ "target": "pose.arms", "access": "exclusive" }],
      "fallback": {
        "pose": "Poses/Restraints/front-cuffs.pose",
        "mask": "bothArmChains",
        "mode": "override"
      },
      "solver": {
        "type": "paired-effectors",
        "chains": [
          { "root": "L_UpperArm", "mid": "L_Forearm", "end": "L_Wrist" },
          { "root": "R_UpperArm", "mid": "R_Forearm", "end": "R_Wrist" }
        ],
        "target": "pairedWrists.front",
        "jointLimits": "human.arms"
      }
    }
  ]
}
```

The exact bones must be resolved through a skeleton-family definition. Missing required bones make
the profile unavailable for that actor; the asserted condition remains true and the owner receives
a structured realization failure. It must not degrade into partial, undefined writes.

## Semantic claims and arbitration

Four pieces of metadata answer different questions:

- a condition says what persistent fact the consumer asserted;
- a capability says what the resulting actor presentation can still do, such as `hands.free` or
  `stride.full`;
- a claim says which semantic pose/motion/attachment target a layer controls and whether that
  access is exclusive or composable; and
- a bone mask says which exact transforms the compositor writes.

Initial standard claim targets should be coarse, hierarchical, and stable:

- `pose.root`, `pose.body`, `pose.upper-body`, `pose.arms`, `pose.arm.left`, and `pose.arm.right`;
- `pose.hands`, `pose.lower-body`, `pose.leg.left`, and `pose.leg.right`;
- `motion.root`, `motion.lower-body`, and `motion.stride`;
- `attachment.hand.left`, `attachment.hand.right`, and `attachment.hip`; and
- explicit contact resources where needed, such as `support.arm.left`.

For example, an exclusive `pose.arms` claim overlaps either individual arm target. A lower-body
locomotion profile does not overlap a front-cuff `pose.arms` profile. Exact masks still decide the
math after these semantic checks pass.

Standard targets and capabilities provide interoperability. Packs may add namespaced extensions
without changing the ABI, but they should not mint a new target when a standard coarse target
describes the same resource.

The claim registry expands every target to a canonical hierarchy. Two claims overlap when either
target is the other or its ancestor. `exclusive` access conflicts with any overlapping writer unless
the scene explicitly `supports` the post-pose constraint or `covers` its condition. `composable`
access is allowed only for declared phases/orders and compatible pose modes. Requirements and
suppressed capabilities are eligibility data, not writers, and do not participate in numerical layer
ordering. Validation also verifies that every weighted bone in a mask is covered by the layer's pose
claim; a semantic right-arm claim cannot conceal an `upperBody` write.

For each state change or scene start, arbitration should:

1. collect routes, asserted conditions, resolved profiles, explicit locomotion, gestures, and scene
   entries;
2. compare claims, requirements, scene compatibility, and consumer enforcement;
3. select an exact variant or constraint target where available;
4. accept, suspend, or reject each entry deterministically;
5. construct the layer plan; and
6. emit a reasoned outcome to affected owners.

Suggested enforcement meanings are:

| Enforcement | Meaning |
| --- | --- |
| required | Do not hide or weaken this condition's realization. Reject an incompatible scene/request. |
| preferred | Keep it when compatible; a policy-selected exclusive presentation may suspend it, then reconcile. |
| cosmetic | May fade or suspend when its claimed resource is needed elsewhere. |

The consumer chooses enforcement. A restraint mod may mark locked handcuffs required. Suit Protocol
can mark a temporary held-helmet gesture preferred, allowing a cinematic scene to settle or suspend
it while the semantic helmet state remains intact.

Enforcement does not grant OSF gameplay preemption authority. If a required assertion arrives while
an incompatible scene is already committed, OSF retains the assertion, reports its realization as
conflicted/pending, and does not silently stop the scene. The consumer may stop the scene, queue the
realization, or apply an instant gameplay-specific fallback. The same assertion present during a new
scene preflight rejects that start under strict policy. This is intentional **committed-holder
precedence**, so the immediate presentation can differ across that commit boundary. It is gameplay
policy rather than registration-order pose math; a consumer that needs required mid-scene preemption
must explicitly stop/replace the scene.

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
outer scene handle, ledger, and synchronization identity. The new low-level playback/node clock
starts for the destination; current registry-authored graph edges use the runtime's default
crossfade because they do not author per-edge blend duration. The target route/scene schema may add
that duration explicitly.

A scene either proves one compatibility contract for every reachable node or preflights each node
transition as a smaller group transaction. A failed navigation remains at the current node or ends
the scene according to explicit scene policy; it never advances only part of the roster. A changed
condition also triggers this preflight before the next node.

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
registered. A restraint integration must choose its policy for legacy/unknown scenes: permissive
generic overlay, temporary suspension, or strict rejection. OSF should report that the match was
unknown rather than silently claim it was compatible.

For the compatibility adapter, a legacy role with no claims is treated as today's exclusive
full-body scene posture. Non-required profile realizations suspend while it plays, but their
conditions remain asserted. A consumer that marks a realization required must explicitly choose
whether unknown legacy scenes are rejected or allowed to use a generic fallback; OSF must not make
that policy choice invisibly.

Equipment stripping is a separate preflight. A required restraint can expose protected forms,
slots, or keywords that the scene's strip plan must retain. A scene that truly requires their
removal is incompatible; merely accepting the restraint's bone constraint is not enough. Existing
`stripActors` behavior remains unchanged for actors with no protected equipment claim.

## Restraints over a scene

There are three useful quality tiers.

| Tier | Technique | Appropriate use | Limitation |
| --- | --- | --- | --- |
| 1 | Full-weight masked pose | Front cuffs in many standing/kneeling poses; simple upright armbinders | Fixed arms can intersect the body, partner, furniture, or floor |
| 2 | Procedural limb constraint/IK | Front/back cuffs whose target moves with pelvis, chest, partner, or furniture | Still cannot repair a base pose that requires those arms for support |
| 3 | Fully authored restrained variant | Load-bearing, close-contact, furniture-bound, or silhouette-critical scenes | Requires additional content |

A tier-1 hard constraint must drive complete limb chains, not just wrists. Blending a cuff pose at
50 percent does not keep the hands together; use blends only while its profile realization enters
or exits.

Tier 2 samples the scene first, resolves an actor/scene-relative target, and then solves both limb
chains with elbow pole vectors, reach limits, and joint limits. A scene author may provide a named
target such as `pairedWrists.front` without authoring an entirely separate animation.

Tier 3 remains necessary when the original animation assumes:

- an arm bears weight or prevents a fall;
- a hand contacts a partner, prop, wall, or furniture;
- behind-the-back arms would intersect the floor or seat;
- an armbinder materially changes the torso silhouette; or
- ankle restraints invalidate the base hip width, balance, or foot placement.

The matchmaker should prefer, in order:

1. an exact authored variant for the active condition set;
2. a compatible scene with an authored constraint target;
3. a scene explicitly allowing the generic constraint fallback; and
4. rejection or an explicit consumer-selected degradation policy.

This avoids both extremes: authoring every possible scene/device combination and pretending a bone
mask can solve every physical interaction.

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
        "supports": ["pairedWrists.front"],
        "constraintTargets": {
          "pairedWrists.front": {
            "space": "role",
            "bone": "C_Hips",
            "translation": [0.12, 0.18, 0.08],
            "rotationDegrees": [0, 0, 0]
          }
        },
        "covers": []
      }
    }
  ]
}
```

This shape is illustrative. `requirements` decides whether the actor may fill the role. `claims`
describes what the playback controls. `supports` opts into a generic post-pose constraint, while
`covers` means the authored clip already depicts a named condition and its overlapping persistent
pose/motion layers should yield. The assertion, suppressed capabilities, and protected device
visuals remain active unless their own ownership contract explicitly transfers them.

Each `supports` entry is the canonical constraint-target ID and must exactly match both its
`constraintTargets` key and the selected profile solver's `target`; there is no implicit string
mapping.

For example, a custom front-cuffed variant would require
`example-restraints.hands.front`, list that same condition in `covers`, and claim its authored arm
pose exclusively. Compatibility and targets are role-local, survive node changes only where
declared valid, and are queryable by matchmaking before the scene starts.

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

An ownership marker produces an ordered, idempotent handoff; it is not magically atomic across a
consumer callback. For two OSF-owned visuals, the route can prepare the destination and ledger both
sides internally. Whenever either endpoint is external, the target API uses a
prepare/acknowledge/commit/compensate protocol:

1. reserve the route resources and ask each external owner to prepare a restorable source or
   destination for the transfer generation, without declaring the transfer committed;
2. create/reveal and acknowledge the destination while the source is still retained or restorable;
3. ask the source owner to hide/release and acknowledge, then journal the irreversible checkpoint
   and commit both endpoints; or
4. on any failure, remove/hide the prepared destination, restore the source, and report a
   compensatable route failure.

Every step is idempotent for the transfer generation. A compatibility adapter over today's void
scene callback can preserve the existing ordered overlap and post-event reconciliation, but cannot
promise rollback of an external mutation.

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
ClearCondition(assertion, requestToken)
SetLocomotionProfile(owner, actor, profileId or none, enforcement, requestToken)
QueryActorPresentation(actor) -> accepted/suspended/failed entries + scene compatibility
ReleaseRoute(routeInstance)
ReleaseOwner(owner)
```

Callbacks/events should include:

```text
owner, actor, instance, requestToken, eventType,
route/station/transition, condition, or resolved profile id,
authored marker id, outcome/reason, current scene handle (if relevant)
```

Callbacks used for an external ownership endpoint must be synchronous on the game thread, as
current native scene cues are, and the proposed prepare/apply callbacks return an
acknowledgement/failure result.
Informational Papyrus events may remain asynchronous. The caller token is echoed for correlation;
only OSF's generation controls at-most-once delivery and stale suppression. Reentrant calls should
be queued and applied after the current dispatch boundary, while start-time events are buffered
until the returned handle is bound, matching the safety property already needed by Suit Protocol.

`ReleaseOwner` is a callback/deferred-command barrier: after it returns, no later dispatch may
reference that owner. A release called reentrantly marks the owner closing immediately, suppresses
further events, and completes logical cleanup after the current callback unwinds. Retired immutable
pose data may remain alive without the owner context until the compose-reader quiescence rule permits
physical reclamation.

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

For persistent constraints, Studio needs:

- named chain definitions and bone validation against the selected rig;
- a fallback pose and mask previewed over arbitrary base clips;
- target and pole-vector gizmos;
- reach, joint-limit, and intersection diagnostics; and
- a way for a scene role/node to publish or override compatible targets.

Profile authoring also needs a `when` condition selector plus standard capabilities and claim
targets. Validation should distinguish an unknown namespaced condition (valid but supplied by an
optional consumer) from an unknown standard target (an interoperability error).

For authored variants, Studio should let an author preview the base scene with active conditions,
then save only the replacement role clip or sparse correction where possible. The registry and
matchmaker need the compatibility metadata regardless of whether the result is a full variant or a
constraint target.

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

If a cinematic scene starts during this preferred gesture, the controller reports suspension or a
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
4. The constraint phase overwrites/solves the complete arm chains after the live base pose.
5. On removal, the mod runs any remove route, clears its condition assertion, and OSF blends the
   profile realization to the live underlying arms.

No custom walk and idle clips are required if the lower body and torso remain plausible. An author
may add speed-specific arm targets or a locomotion profile for better quality.

### Handcuffs during an intimate multi-actor scene

1. The matchmaker queries each actor controller and sees a required front-cuff condition and its
   resolved presentation requirements.
2. It selects an authored cuffed variant, a scene with a paired-wrist target, or an explicitly
   generic-compatible scene.
3. `SceneRuntime` submits the synchronized scene body clip as the actor's base scene layer.
4. The cuff solver samples that result and constrains the wrists/arm chains to the scene target.
5. Cuff pieces follow the final wrist transforms; a connector follows both endpoints.
6. When the scene ends, the scene body layer disappears. The still-asserted condition reselects its
   profile over Starfield locomotion.

If the scene uses the actor's arms for support, the generic constraint fallback is rejected and an
authored restrained variant is required. No blend weight can repair the missing support assumption.

### Ankle cuffs

1. The device mod asserts its paired-ankle condition; the matching profile selects an ankle-cuffed
   locomotion profile.
2. In ordinary play, the profile supplies narrower idle/walk/run visuals while Starfield still owns
   movement and world translation.
3. A cinematic scene that authors a sufficiently narrow lower-body pose may accept the condition
   without the locomotion profile.
4. A wide stance or load-bearing foot placement requires a compatible variant or rejection.

## Interruption, failure, and reconciliation

The runtime must make failure observable and leave semantic ownership unambiguous.

- **Missing clip or invalid rig:** reject before committing the destination and report a structured
  reason. The consumer chooses an instant visual fallback or retains its previous presentation.
- **Countermand:** advance OSF's transition generation, follow the authored reversal policy, and
  suppress stale work while echoing the consumer's token only as correlation data.
- **Cinematic conflict:** required condition realizations reject incompatible starts;
  preferred/cosmetic entries may suspend and later reconcile.
- **Actor 3D unavailable:** retain desired state without sampling/stamping. Rebind and reconstruct
  when a valid skeleton returns.
- **Owner release/plugin unload:** remove only that owner's entries and clean its OSF-owned props.
- **Scene abort:** release all scene body layers through the existing ledger end path, then reconcile
  controllers.
- **World-replacing load:** clear every runtime instance and raw actor reference. Consumers rebuild
  from persisted semantic state; in-flight transitions are never restored.
- **Death, furniture, combat, or menus:** OSF exposes presentation availability and reasons; each
  consumer decides whether its request is required, deferred, instant, or cancelled.

Reached-station and ownership-marker callbacks must be idempotent. A consumer should be able to
reassert its current desired station/condition at any time without duplicating a prop or replaying a
completed transition.

## Delivery plan

### Phase 0: formalize assets and contracts

- Keep the current Suit Protocol implementation working.
- Specify route runtime data from Studio's existing station/transition model.
- Define standard claims, skeleton-family masks, request outcomes, and compatibility metadata.
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

This phase makes generic masked restraint poses possible over compatible scenes.

### Phase 3: procedural constraints and scene compatibility

- Add human limb-chain definitions, paired-effector solving, joint limits, and pole targets.
- Add role/node constraint targets and matchmaker compatibility queries.
- Add exact-variant preference and structured rejection/degradation outcomes.
- Add Studio preview and diagnostics for constraints over scene clips.

This phase provides the practical middle tier between a fixed cuff pose and custom animation for
every scene.

### Phase 4: locomotion profiles and richer composition

- Add generic movement parameters and a small idle/walk/run profile resolver.
- Add lower-body device profiles and transition rules.
- Extend claims, masks, and constraint types only from demonstrated content needs.
- Consider multiple soft gesture/additive channels after two real integrations need them.

## Compatibility and migration

- Existing `*.osf.json` scenes remain valid and retain current defaults.
- Existing scene native/Papyrus handles remain scene handles; actor-route handles use a separate API
  and namespace.
- Existing role mask/`preserveBones`/mode/weight settings lower into a scene body layer without
  semantic change.
- Current scene graph nodes and timed lanes remain the orchestration source during migration.
- Suit Protocol can migrate transition by transition. Its transactional core, persistent hip clone,
  and load reconciliation remain authoritative.
- A device mod can first use a generic persistent upper-body pose, then adopt constraints and
  variants as those phases land.

Internally, `GraphManager` can initially adapt the old one-graph path into the compositor. A large
rename is not a prerequisite. The invariant to establish early is that only the compositor writes
the final actor pose.

## Performance and safety constraints

- Capture and convert the live actor pose once per actor per frame.
- Sample only active layers; cache static station/constraint fallback poses.
- Stamp the flat rig buffers once after all composition.
- Bound active layers and solver iterations; log rejected overflow rather than degrade unpredictably.
- Keep arbitration event-driven. Only movement parameters and active clip/solver sampling belong in
  the frame path.
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
4. A compatible multi-actor scene and a required cuff constraint compose in one deterministic final
   pose.
5. An arm-supported incompatible scene is rejected or selects an authored variant rather than
   silently breaking the pose.
6. Ankle cuffs can select a restricted locomotion profile without OSF taking ownership of gameplay
   speed.
7. Every ownership transfer shows exactly the intended visual through completion, countermand,
   abort, actor 3D rebuild, and load reconciliation.
8. With no actor routes, assertions, or profiles active, legacy scene output and timing are
   unchanged.
9. Layer registration order cannot change the final pose.
10. Tests cover arbitration, masks, composition order, transition markers, stale request tokens,
    suspension/reconciliation, cleanup, and transactional multi-role scene start.

## Open design decisions

The architecture does not depend on these spellings, but implementation must settle them before the
new schema/API is public:

- whether route/presentation-profile/locomotion assets live in the unified scene file or separate
  registries;
- the standard claim namespace and how packs extend it;
- the exact compatibility policy for legacy scenes when a consumer does not specify one;
- the canonical actor-/scene-local target spaces and their conversion into the model-space solver;
- how cross-actor/furniture targets avoid evaluation cycles;
- whether route reversal can scrub a transition backward or always follows authored reverse clips;
  and
- how two-parent connectors such as chains are rendered and culled.

The defaults recommended by this RFC are conservative: explicit reverse clips, actor-local targets,
consumer-selected legacy policy, one cinematic scene per actor, required restraints never silently
suspended, and one final pose stamp.
