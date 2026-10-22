# Node catalog and execution

The additive `nodes` array in catalog schema 3 exposes every resolved node, including graph nodes and synthetic linear stages. Its address is the exact `(scene, id)` pair; IDs are not flattened into a delimiter-based string. The containing scene supplies roles, placement, anchor requirements and source provenance. An Intimacy published node ID remains a separate semantic binding to this motion address.

Graph nodes may declare `name`, `tags`, `priority`, positive `weight`, and `unlisted`. Tags are a case-insensitive union with scene tags. Ranking and visibility inherit the scene when omitted. Synthetic linear nodes inherit their stage name and tags. `clipsAvailable` checks every segment of that node and its cast size; a missing clip elsewhere in the route does not hide a usable node. Scene-level discovery retains its existing conservative availability rule.

`stage` is the linear index, or -1; `entry` identifies the authored route entry; `segments` counts internal timeline segments. These are discovery records, not new scene definitions: starting at a node retains the original route, cues and outgoing edges.

Scene ABI **1.7** appends borrowed `OSFStartOptions.startNode`. `StartScene` and `StartSceneRoles` copy and validate it before preparation. Unknown nodes or a simultaneous nonzero `startStage` refuse instead of falling back; omitted options preserve existing starts. Matchmade scene starts continue using the authored entry.

Services **3.15** appends `MatchNodeCast`, `MatchNodeAnchor`, and `ReplaceNodeRoles`. The cast query returns exact address pairs for every available node compatible with the ordered cast. It includes unlisted nodes so explicit callers can discover them; consumers filter catalog tags, visibility, ranking and caller authority. The anchor query returns compatible scene IDs without disqualifying usable nodes for a missing sibling clip. Both require Animation's verified frame context. `ReplaceNodeRoles` copies the exact address and role assignment, and retains the existing asynchronous preparation, rollback and optional covered handoff. It returns acceptance, not playback success. Use authored `Navigate`/`Advance` for seamless in-route edges.

The scene ABI append preserves older stamped option sizes. The services append preserves earlier table prefixes. Existing scene catalog and matching APIs retain their behavior. Catalog schema 3 remains additive: older readers may ignore `nodes`.
