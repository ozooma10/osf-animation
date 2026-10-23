# Node catalog and execution

Animation 2.0 publishes catalog schema **4** with one `nodes` array. Each globally unique,
case-insensitive ID owns its name, tags, priority, weight, visibility, cast, placement, anchor,
source provenance and ordered `stages`. A stage is an actual playback segment, never another node.
No scene container or synthetic linear ID is published. File defaults resolve during loading;
tags union case-insensitively and node scalar/role declarations replace defaults.

`clipsAvailable` checks the complete node timeline. Matching ranks the resulting node metadata.
Consumers that only want route entries must select explicit IDs or entry tags; entry is not a
second registry concept. Intimacy's published semantic ID is a separate binding to one motion ID.

Scene ABI **2.0** `StartScene`/`StartSceneRoles` accept the global node ID and an optional initial
stage. `GetNode(handle)` reports that ID; stage getters report the current segment. Services **4.0**
`MatchCast`/`MatchAnchor` return global-ID arrays. Cast matching is positional and includes unlisted
nodes for explicit consumer selection; ordinary tag matchmaking still respects visibility/ranking.

`ReplaceNode(handle, nodeId, roles, count, options)` selects an exact destination and role binding.
`OSFTransitionOptions` supplies stage, optional anchor/heading and `covered`. Authored edges and
exact requests share destination admission, preparation, ownership, rollback and completion.
True means accepted, not committed; observe events and `ReadScenePreparation`.

Authored routes retain their immutable source publication across reload/provider withdrawal.
Exact requests use the current publication. Final publication validation includes generated and
provider nodes, rejects missing targets and dependent routes, and preserves valid cycles.
Provider registration/removal can restore/reject these routes without reparsing files. Existing
NodeRefs remain valid. A provider receives base candidates, never its previous output.

Full catalog source clips use global `node`, `stage`, `clip` offsets. Compact provider input schema
**2** carries the same source addresses and node metadata. Catalog reads and provider callbacks use
cached clip availability and do no resource IO. The Browser's typed/streamed parsers use these new
schemas; the `activeLaunches` web event carries current global node IDs.

Native clients must rebuild against scene 2.0/services 4.0; previous majors and old prefix layouts
are rejected. The persistence C ABI remains unchanged.
