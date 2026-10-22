# Animation 2.0 migration

Implemented for the unpublished 2.0 candidate. The registry, runtime transitions, public adapters,
Browser, Intimacy and Seduce consumers use global node IDs. Production and private probe builds
and the offline suites pass; live qualification remains open. This records the choices for
V2_SCOPE A-C. See [verification and acceptance](RELEASE_2.0.0_ACCEPTANCE.md).

## Registry and addresses

A registry entry is one node with a globally unique, namespaced ID. A running scene is still a
handle owning participants, playback, preparation and cleanup; it is not a registry container.
The handle's current node ID is its only content address. A stage index addresses a playback
segment inside that node, never another node.

Schema 2 files contain `nodes`, or use the bare `id` plus `clip`/`stages` shorthand for one node.
A file may supply metadata, roles, placement and policy defaults. Each node resolves those defaults
at load: scalar values replace defaults, roles replace the default cast, and tags form a stable
case-insensitive union with file tags first. Priority, weight, visibility and clip availability
belong to the resulting node and are used directly by matchmaking. Pack and folder remain catalog
grouping metadata, not address components inferred by the runtime.

```json
{
  "schema": 2,
  "name": "Example pack",
  "tags": ["example"],
  "roles": [{"name": "player"}],
  "nodes": [
    {
      "id": "example/wave",
      "name": "Wave",
      "stages": [{"clips": ["Example/wave.glb"]}],
      "edges": [
        {"when": "end", "to": "example/idle"},
        {"id": "change", "label": "Change pose", "to": "other/pose", "covered": true}
      ]
    },
    {"id": "example/idle", "loops": 0, "stages": [{"clips": ["Example/idle.glb"]}]}
  ]
}
```

Routes are the edges over these node IDs. They do not own nodes or create a second address space.
An edge may name a node in another file or pack. `$end` retains its terminal meaning. Missing
targets are diagnosed and cannot silently turn into another destination. No route search is added.
The old nested `scenes`/`nodes` container, `entry`, `linearStages`, synthetic `#s` node addresses,
and node `use` splice retire. Prop-template `use` is unrelated and remains supported.

A node's `stages` list remains one timeline, so the short authoring form does not expand into a
chain of hidden nodes. Authors who need a boundary with a choice, policy change, or node-scoped
track write explicit nodes and edges. Existing linear content with stage-scoped tracks migrates
to explicit nodes to preserve its boundary behavior.

## Transitions

Authored edges and exact caller requests use the same destination admission and preparation
path. That path validates the retained source revision, destination, cast, role mapping, placement
and anchor before mutation; it retains the existing equipment ownership and provider commit guards.
Cover is a boundary requirement, represented by `covered`, not a different executor. Seamless
boundaries do not acquire cover. Covered boundaries retain the wall-clock deadline and stamped-pose
reveal gate. Unavailable cover reports `transition-cover-unavailable` through preparation status.

A refusal before source mutation can resume an untouched manual source. A consumed automatic
boundary cannot be replayed and terminates on refusal. Committed resource claims survive failures
that happen before a new preparation attempt has acquired any provisional reservations.

## ABI and spelling changes

Scene ABI 2.0 and services ABI 4.0 each expose one baseline table. Clients and hosts reject the
previous majors; the special services-3.13 negotiation branch and old prefix table are removed.
Fields and methods may still append within the new majors under the existing stability policy.

Authoring uses `playerInputLock`, `hideApparel`, `placement`, `sceneControls` and `attachmentNode`.
Their retired aliases produce migration diagnostics. Browser records use `sourceKind`; the active
launch event is `osf.animation.activeLaunches`. The `Stage`/`StageData` C++ compatibility aliases
are removed; playback types use `Segment`/`SegmentData`.

Holding is caller navigation policy. `allowManualHold` is no longer a content grant.
`SetSceneAutoAdvance(false)` suppresses automatic boundaries while sampling and cues continue;
explicit navigation still works. Intimacy enforces its own navigation window and preparation state.

## Layers and verification

The engine-free depth-1 composer delegates to the existing pose helpers and refuses deeper stacks
without writing. Its suite compares exact bytes, including additive pads and scale. The runtime
stamp hook and live-base capture remain unchanged; multiple simultaneous layers are a later 2.x task.

Acceptance for this change is builds and the pure suites. No MO2 deployment, game launch, visual
acceptance, audio/controller acceptance, or fresh save/load qualification is implied by those checks.
