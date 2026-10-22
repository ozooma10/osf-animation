# Upgrading to OSF Animation 2.0.0

2.0.0 is a breaking, unreleased development update. Update the engine, optional Browser, Intimacy,
Seduce and their source content/scripts together. No package publication or game qualification is
implied by the offline builds. See [acceptance](RELEASE_2.0.0_ACCEPTANCE.md).

- Rebuild native consumers against scene ABI 2.0 and services ABI 4.0. Previous majors, prefix
  layouts and startNode/scene-node pair APIs are rejected. The persistence C ABI stays unchanged.
- Convert `*.osf.json` to schema 2: top-level `nodes[]` or bare `id` plus `clip`/`stages`. Give every
  node a global ID and retarget edges to those IDs. Inline node `use` sources. Remove containing
  scene IDs, `entry`, `linearStages` and `allowManualHold`. Keep one timeline per node; split stages
  with their own tracks into explicit nodes. Tag only intended start nodes for entry selection.
- Use canonical policy/prop fields. Recompile Papyrus against `HideApparelMode`, `PlayerInputLockMode`,
  `SceneControlsMode`, `FollowActorMode`, and events with global `node`. `GetNode` replaces GetSceneId;
  stage APIs now address actual segments. Bit 64 is `EVENT_NODE_TRANSITION`.
- Catalog schema is 4; compact provider input is 2. MatchCast/MatchAnchor return global-ID arrays.
  ReplaceNode uses stage/anchor/heading/covered options; cover is a boundary property.
- Intimacy schema-2 bindings use `motion: {node: globalId}`. Published semantic IDs remain distinct.
  Schema-1 private preparation metadata uses flat `nodes` keyed by global motion ID.
- Migrate alignment overrides to formatVersion 2 global node/role keys. Unsupported old files are
  preserved, diagnosed and blocked from saving; there is no lossy automatic remapping. Persisted
  choices for removed content remain unavailable until their IDs are updated.

For a future installation, replace old engine/Browser payloads rather than merging incompatible
files. Preserve custom content, saves and user preferences first. Browser remains optional and needs
OSF UI 2.0.0+ and OSF Settings 1.0.0+; Settings owns hotkeys and its launcher. Rebind hotkeys if coming
from the former combined settings provider. No installation is performed by this migration task.

The layer composer implements depth 1 and refuses deeper stacks. Graph.cpp, StampPose and runtime
pose stamping are unchanged; simultaneous layer playback remains a later 2.x feature.
