# OSF domain vocabulary

These terms describe the Animation 2.0 contract. Content uses schema 2 and canonical field
spellings. Track positions still serialize as `at`/`atFrame`; concept names are not automatic API keys.

## Runtime and topology

| Canonical term | Meaning | Terms to qualify or avoid |
|---|---|---|
| **scene** | A live handle owning participants, current node, preparation, playback and undo ledger. | Do not use for every low-level animation playback object. |
| **playback session** | The Layer-A synchronized animation playback used by a scene. | C++ uses `PlaybackSession`, `PlaybackPlan`, and `PlaySynchronized`. |
| **flow node** | A globally addressable registry entry with its own timeline, roles, policy, tracks and outgoing edges. | Use bare *node* only when the flow context is unambiguous. An actor attachment node or engine render node is a different concept. |
| **attachment node** | A named actor skeleton/render-tree target for a prop attachment. | Preferred prop field `attachmentNode`; only `attachmentNode` is accepted. It does not identify a flow node. |
| **actor playback graph** | The per-actor animation sampler/stamper used by the playback layer. | Current C++ type `Animation::Graph`; do not shorten this to *scene graph*. |
| **render node / render tree** | An engine `NiNode`/`BGSModelNode` and its hierarchy. | Current utility name `SceneGraph` refers to this engine render tree, not scene flow. |
| **stage** | A zero-based `stages[]` segment inside one node. | Public stage getters/setters address this segment; they do not select another node. |
| **playback segment** | One low-level unit of clips, timing, placement, and blend inside a playback session. | Preferred C++ names are `PlaybackPlan::Segment`/`SegmentData`; the `Stage`/`StageData` compatibility aliases are removed. |
| **scene edge** | A directed connection between scene flow nodes (`edges[]`). | Edges determine scene navigation. |

## Policy and authoring

| Canonical term | Meaning | Compatibility spelling |
|---|---|---|
| **player input lock** | Suppression of the player's normal movement/combat input while participating. | Preferred JSON `playerInputLock`; Papyrus `PlayerInputLockMode`; action `osf.control.lock`. |
| **scene controls** | OSF's advance, navigate, speed, alignment, freecam, and end commands. | Preferred JSON `sceneControls`; Papyrus `SceneControlsMode`. This is distinct from the player input lock. |
| **hide apparel** | Temporarily hide or unequip worn apparel while retaining the actor's base body and restoring recorded items later. | Preferred JSON `hideApparel`; Papyrus `HideApparelMode`; `HideEquipment`. |
| **world anchor** | A spatial reference and transform used to place a scene and its participants. | JSON `anchor`; Papyrus `SceneOptions.Anchor`; solo `SetAnchor`. |
| **world placement** | The policy for placing participants relative to their live world transforms. | Preferred JSON `placement` with `"anchorAndPin"` or `"followActor"`; Papyrus `FollowActorMode`. |
| **track position** | A temporal mark such as `enter`, `exit`, `end`, a clip fraction, or a frame. | JSON `at`/`atFrame`; event ABI field `anchor` is a legacy name for a named track position, not a world anchor. |
| **animation clip spec** | An unresolved author-facing animation reference, including its resource path and optional GLB animation id. | JSON commonly serializes it through `clip`, a `clips[]` entry, or an object with `file` and `anim`. |

Prefer `placement: "followActor"` for a scene whose playback follows each actor's live world transform
and `placement: "anchorAndPin"` for pinned behavior. Role pose composition is independent of
world placement. There is no legacy `inPlace` alias.

## Content and tools

| Canonical term | Meaning | Compatibility spelling |
|---|---|---|
| **content registry** | The loaded snapshot of global nodes, curated animation entries, and per-file import results. | Internal types use `ContentRegistry`, `ContentRegistrySnapshot`, `ContentFileStats`, and `ContentImportProblem`. `GetSceneLoadErrors` remains the public Papyrus spelling. |
| **content file** | One discovered `*.osf.json` document and the defaults or reusable definitions scoped to that file. | Do not call a file-level default *pack-wide*. |
| **content pack** | An author/product grouping that may span multiple content files; `pack` is its presentation label. | It is not a parser or inheritance boundary. `ReloadPacks` is a legacy binding name for reloading all content. |
| **playable catalog** | The umbrella collection of browsable scenes, emotes, and animations that can be launched. | Avoid using *library* for every catalog partition. |
| **catalog source kind** | The origin taxonomy carried by browser catalog records: `authoredScene`, `curatedAnimation`, `providedScene`, or `referenceAnimation`. | Prefer `sourceKind` over inferring origin from an id prefix, the catalog/library channel, or the legacy `curated` boolean. |
| **curated animation** | An explicitly registered, individually browsable animation clip. | Established JSON key `clipLibrary`; it is distinct from an automatically derived inspection entry. |
| **reference catalog** | Non-matchmaking reference content selected by file-level `section: "library"`. | `library` is the established schema value, not the umbrella term for all playable content. |
| **emote** | A playable solo gesture/category, such as content tagged `player.emote.*`. | Reserve *action* for entries on the authored `action` track; older UI copy may label emotes `Action`. |
| **active launch** | A UI projection with a handle that represents a live scene. | Preferred browser type/function `ActiveLaunch`/`activeLaunches`; the web event is `osf.animation.activeLaunches`. Do not infer that every entry is a Layer-A playback session. |

## Casting and lifecycle

| Canonical term | Meaning | Terms to avoid or qualify |
|---|---|---|
| **role** | An authored requirement and position in a node definition. | Avoid bare *slot* for a role; slot may also mean a handle-table slot. |
| **cast** | Actors supplied or selected before role binding. | Retire *crew* for this concept. |
| **role binding** | The mapping from cast actors to authored roles. | — |
| **participant** | A role-bound actor in a live scene. | Do not use for an actor that has not entered the scene. |
| **roster** | The ordered collection of a scene's participants. | — |
| **complete** | Reach the authored natural end. | Use instead of an unqualified *finish*. |
| **cancel** | End because a caller or user requested a stop. | Public `Stop*` API names remain valid. |
| **interrupt** | End because external runtime state prevented continuation, such as world replacement or playback failure. | — |
| **terminate** | Umbrella term for complete, cancel, or interrupt. | Use only when the cause is intentionally unspecified. |
| **teardown** | Release runtime resources and reverse ledgered mechanisms after termination. | Teardown is cleanup, not a termination cause. |

Scene-end events carry a complete/cancel/interrupt cause and the global node ID at dispatch.
Some interruptions, notably world replacement, intentionally emit no
callback into the discarded world. Consumers that require proof of natural completion must continue
to use an authored completion cue.
