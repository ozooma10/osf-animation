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

## What happens to saved preferences

OSF Settings loads and saves engine values under `osf.animation.engine`, using schema defaults
when no value is saved. Existing values under that ID continue to load normally. Old combined
OSF UI/Settings values and `Data/OSF/settings.json` are no longer read. Rebind the browser and
wheel hotkeys after moving from the combined provider.

The Browser's OSF Settings page now contains only its two hotkeys. Change Browser preferences
inside the Browser; detail/source filters sit beside Browse. Browsing state is always remembered
within a game session. The saved speed default and unused stage-transition popup setting have
been removed. Launches start at 1x unless you change Start Overrides for that launch; keyboard
speed controls still work during playback. Playback and Alignment bindings have separate groups.
Browser preferences load from `SFSE/OSF/browser-preferences.json` under the game's documents
folder, or use defaults when that file is absent. Changes are saved there.

## First launch

- Confirm **OSF Animation** and, when installed, **OSF Animation Browser** appear
  in OSF Settings. Check **Mod Issues** for missing content or incompatible hosts.
- Open Browser from the Launcher tab, check your installed collections, and try
  both rebound hotkeys. Confirm saved wheel assignments still resolve.
- Start and end a familiar scene. Check that input, camera, and equipment return
  as expected, then check save/load in your normal mod setup.

These checks cover your installation and upgrade. Automated installer tests verify
file layout and preservation; the disposable game suite does not replace a real
upgrade or visual, audio, and physical-controller acceptance. See
[testing coverage](SMOKE_TESTING.md#coverage-boundaries).
