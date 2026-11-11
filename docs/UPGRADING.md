# Upgrading to OSF Animation 2.0.0

2.0.0 is an unreleased development update with a [published 1.4 compatibility layer](COMPATIBILITY_1.4.md).
That layer still needs live qualification and retains 2.0 asynchronous admission. Update the engine, optional Browser, Intimacy,
Seduce and their source content/scripts together. No package publication or game qualification is
implied by the offline builds. See [acceptance](RELEASE_2.0.0_ACCEPTANCE.md).

For current development consumers and schema-2 authoring:

- Rebuild native consumers against scene ABI 2.0 and services ABI 4.0. Unpublished previous majors, prefix
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

When installing the candidate, replace old engine/Browser payloads rather than merging incompatible
files. Preserve custom content, saves and user preferences first. OSF Settings 1.0.0+ provides engine
settings. Browser remains optional and additionally needs OSF UI 2.0.0+; Settings owns its hotkeys
and launcher. Install matching engine/Browser 2.0.0 archives and rebind the two Browser hotkeys when
coming from the former combined settings provider.

The engine-free layer composer tests depth 1 and refuses deeper stacks. It is not wired into
runtime stamping; simultaneous OSF layer playback remains a later 2.x feature.

## What happens to saved preferences

OSF Settings loads and saves engine values under `osf.animation.engine`, using schema defaults
when no value is saved. Existing values under that ID continue to load normally. Old combined
OSF UI/Settings values and `Data/OSF/settings.json` are no longer read. Rebind the browser and
wheel hotkeys after moving from the combined provider.

The Browser's OSF Settings page now contains only its two hotkeys. Change Browser preferences
inside the Browser; detail/source filters sit beside Browse. Browsing state is always remembered
within a game session. The saved speed default and unused stage-transition popup setting have
been removed. Launches start at 1x unless you change Start Overrides for that launch; keyboard
speed controls still work during playback. The engine page contains scene gear, six playback
bindings and log level. Twelve alignment bindings now live on the separate `osf.animation.align`
page; recheck any custom alignment bindings previously stored under the engine ID. Top-row `=` / `-`
are fixed speed aliases, with explicit bindings taking precedence. Browser always reopens on its
last tab; unavailable entries stay behind the list's reveal button. Retired `openTo` and
`unavailableScenes` preferences are ignored.
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
