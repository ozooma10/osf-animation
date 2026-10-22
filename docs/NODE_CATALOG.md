# Node catalog and execution

The additive `nodes` array in catalog schema 3 exposes every resolved node, including graph nodes and synthetic linear stages. Its address is the exact `(scene, id)` pair; IDs are not flattened into a delimiter-based string. The containing scene supplies roles, placement, anchor requirements and source provenance. An Intimacy published node ID remains a separate semantic binding to this motion address.

Graph nodes may declare `name`, `tags`, `priority`, positive `weight`, and `unlisted`. Tags are a case-insensitive union with scene tags. Ranking and visibility inherit the scene when omitted. Synthetic linear nodes inherit their stage name and tags. `clipsAvailable` checks every segment of that node and its cast size; a missing clip elsewhere in the route does not hide a usable node. Scene-level discovery retains its existing conservative availability rule.

`stage` is the linear index, or -1; `entry` identifies the authored route entry; `segments` counts internal timeline segments. These are discovery records, not new scene definitions: starting at a node retains the original route, cues and outgoing edges.
