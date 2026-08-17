# OSF domain vocabulary

Use these terms in new code, documentation, tests, diagnostics, and UI copy. Existing JSON keys,
Papyrus functions, and native ABI fields remain valid compatibility spellings; this vocabulary does
not change their behavior.

The canonical JSON keys named below are accepted by schema version 1 alongside their legacy aliases.
Author only one spelling from each canonical/legacy pair in a given object; using both is a load
error. Other canonical concept names are **not automatically schema or API names**: for example,
track positions still serialize as `at`/`atFrame`, not `trackPosition`.

## Runtime and topology

| Canonical term | Meaning | Terms to qualify or avoid |
|---|---|---|
| **scene** | An authored scene definition or its live domain runtime: roles, flow, policy, tracks, participants, and undo ledger. | Do not use for every low-level animation playback object. |
| **playback session** | The Layer-A synchronized animation playback used by a scene, route transition, or station. | `Animation::Scene`, `ScenePlan`, and `PlaySceneStaged` remain compatibility aliases/wrappers for `PlaybackSession`, `PlaybackPlan`, and `PlaySynchronized`. |
| **flow node** | A node in a scene's authored control-flow graph (`nodes[]`). | Use bare *node* only when the flow context is unambiguous. An actor attachment node or engine render node is a different concept. |
| **attachment node** | A named actor skeleton/render-tree target for a prop attachment. | Preferred prop field `attachmentNode`; legacy alias `node`. Neither identifies a flow node. |
| **actor playback graph** | The per-actor animation sampler/stamper used by the playback layer. | Current C++ type `Animation::Graph`; do not shorten this to *scene graph*. |
| **render node / render tree** | An engine `NiNode`/`BGSModelNode` and its hierarchy. | Current utility name `SceneGraph` refers to this engine render tree, not scene flow. |
| **linear stage** | One author-facing entry in a linear scene's `stages[]`, or one entry exposed by `linearStages`. | A linear-stage index is not a playback-segment index. |
| **playback segment** | One low-level unit of clips, timing, placement, and blend inside a playback session. | Preferred C++ names are `PlaybackPlan::Segment`/`SegmentData`; `Stage`/`StageData` remain compatibility aliases. |
| **scene edge** | A directed connection between scene flow nodes (`edges[]`). | Do not call a route transition an edge. |
| **route transition** | A directed connection between route stations (`transitions[]`). | Algorithmic prose should say *transition count*, not *edge count*. |

## Policy and authoring

| Canonical term | Meaning | Compatibility spelling |
|---|---|---|
| **player input lock** | Suppression of the player's normal movement/combat input while participating. | Preferred JSON `playerInputLock`; legacy `lockPlayer`; Papyrus `LockPlayerMode`; action `osf.control.lock`. |
| **scene controls** | OSF's advance, navigate, speed, freecam, and end commands. | Preferred JSON `sceneControls`; legacy `playerControl`; Papyrus `PlayerControlMode`. This is distinct from the player input lock. |
| **hide apparel** | Temporarily hide or unequip worn apparel while retaining the actor's base body and restoring recorded items later. | Preferred JSON `hideApparel`; legacy `stripActors`; Papyrus `StripMode`; `HideEquipment`. |
| **world anchor** | A spatial reference and transform used to place a scene and its participants. | JSON `anchor`; Papyrus `SceneOptions.Anchor`; solo `SetAnchor`. |
| **world placement** | The policy for placing participants relative to their live world transforms. | Preferred JSON `placement` with `"anchorAndPin"` or `"followActor"`; legacy `inPlace` with `false` or `true`; Papyrus `InPlaceMode`. |
| **track position** | A temporal mark such as `enter`, `exit`, `end`, a clip fraction, or a frame. | JSON `at`/`atFrame`; event ABI field `anchor` is a legacy name for a named track position, not a world anchor. |
| **animation clip spec** | An unresolved author-facing animation reference, including its resource path and optional GLB animation id. | JSON commonly serializes it through `clip`, a `clips[]` entry, or an object with `file` and `anim`. |

Prefer `placement: "followActor"` for a scene whose playback follows each actor's live world transform
and `placement: "anchorAndPin"` for the default pinned behavior. The legacy mappings are
`inPlace: true` and `inPlace: false`, respectively. In prose, say **follow-actor placement** versus
**anchor-and-pin placement**; avoid treating *in place* and *anchored* as simple opposites without
describing the behavior.

## Content and tools

| Canonical term | Meaning | Compatibility spelling |
|---|---|---|
| **content registry** | The loaded snapshot of scenes, routes, curated animation entries, and per-file health problems. | Preferred internal aliases `ContentRegistry`/`ContentRegistrySnapshot`; `SceneRegistry`/`SceneRegistrySnapshot` and `GetSceneLoadErrors` remain compatibility names. |
| **content file** | One discovered `*.osf.json` document and the defaults or reusable definitions scoped to that file. | Do not call a file-level default *pack-wide*. |
| **content pack** | An author/product grouping that may span multiple content files; `pack` is its presentation label. | It is not a parser or inheritance boundary. `ReloadPacks` is a legacy binding name for reloading all content. |
| **playable catalog** | The umbrella collection of browsable scenes, emotes, and animations that can be launched. | Avoid using *library* for every catalog partition. |
| **catalog source kind** | The origin taxonomy carried by browser catalog records: `authoredScene`, `curatedAnimation`, or `referenceAnimation`. | Prefer `sourceKind` over inferring origin from an id prefix or catalog/library channel. |
| **curated animation** | An explicitly registered, individually browsable animation clip. | Established JSON key `clipLibrary`. |
| **reference catalog** | Non-matchmaking reference content selected by file-level `section: "library"`. | `library` is the established schema value, not the umbrella term for all playable content. |
| **emote** | A playable solo gesture/category, such as content tagged `player.emote.*`. | Reserve *action* for entries on the authored `action` track; older UI copy may label emotes `Action`. |
| **active launch** | A browser projection of a live scene handle. | Preferred browser type/function `ActiveLaunch`/`activeLaunches`; native event `activeScenes` remains the compatibility wire name. |

## Casting and lifecycle

| Canonical term | Meaning | Terms to avoid or qualify |
|---|---|---|
| **role** | An authored requirement and position in a scene definition. | Avoid bare *slot* for a role; slot may also mean a handle-table slot. |
| **cast** | Actors supplied or selected before role binding. | Retire *crew* for this concept. |
| **role binding** | The mapping from cast actors to authored roles. | — |
| **participant** | A role-bound actor in a live scene. | Do not use for an actor that has not entered the scene. |
| **roster** | The ordered collection of a scene's participants. | — |
| **complete** | Reach the authored natural end. | Use instead of an unqualified *finish*. |
| **cancel** | End because a caller or user requested a stop. | Public `Stop*` API names remain valid. |
| **interrupt** | End because external runtime state prevented continuation, such as world replacement or playback failure. | Route `interrupt: "finish"` is different: that legacy value means complete the current route transition before honoring the new request. |
| **terminate** | Umbrella term for complete, cancel, or interrupt. | Use only when the cause is intentionally unspecified. |
| **teardown** | Release runtime resources and reverse ledgered mechanisms after termination. | Teardown is cleanup, not a termination cause. |

When current scene-end event ABIs emit an event, they report termination but do not carry a
complete/cancel/interrupt cause. Some interruptions, notably world replacement, intentionally emit no
callback into the discarded world. Consumers that require proof of natural completion must continue
to use an authored completion cue.
