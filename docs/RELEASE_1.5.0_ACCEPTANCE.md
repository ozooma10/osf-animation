# OSF Animation 1.5.0 acceptance

1.5.0 remains unpublished. Record the candidate source fingerprint, engine/Browser
ZIP hashes, host versions and observations together. A passing automated report
does not mark the hands-on rows below as passed. Keep dated execution results in
the candidate's evidence directory instead of treating this checklist as a receipt.

Run `pwsh -NoProfile -File tools/test-release.ps1 -Interactive` for the released
UI/Settings hosts. Use `-Hosts Deployed` for a separate compatibility run against
the current development installation. Both use disposable profiles and saves.
The runner retains packages, installer results, native snapshots and captures.
Do not replace an unrelated running game or MO2 session to run the suite.

| Check | Automated evidence | Remaining acceptance |
| --- | --- | --- |
| Packaging and installation | Both ZIP contents/hashes; five FOMOD choices installed twice; unrelated files preserved | Install the candidate through the actual mod manager |
| 1.4 upgrade | Replacement layout and retained user-file checks | Upgrade a copied 1.4 profile, rebind hotkeys, confirm preferences, content and wheel pins, exit normally and restart |
| Core without Browser/UI | Package separation and browser/engine API boundary | Launch a consumer scene in a copied core-only profile; no issue merely for missing optional components |
| Handoff and alignment | Query/role mapping, paused revisits, exact event identities, private alignment save/cache reload/new-scene readback and reset | Judge visible blend, positional snap, furniture placement and persistence after a full process restart |
| Progression | Manual hold keeps cues/sampling alive; pause/manual preference survives transitions/handoff; automatic completion resumes | Exercise the intended consumer's live controls |
| Restoration | Actor positions, apparel, input and camera after role-swapping completion and explicit stop; save during active scene and quickload | Real consumer cancellation/recovery, failed/cancelled loads and unusual equipment/weapon conflicts |
| Browser and wheel | Document/catalog/import/wheel lifecycle and trusted Imports click | Keyboard/controller launcher and hotkey matrix, wheel edits/pins after restart, input/cursor return |
| Presentation | Finite stamped transforms, clock synchronization, Wwise acceptance, bounded fade/subtitle state | Watch multi-actor sync, seams/culling/props/fades; listen to audio; use a physical controller; inspect representative creature rigs |

Use a disposable copy of the user's mod setup for upgrade and restart checks.
Preserve original saves/sidecars, settings, pins and custom content. Record a
failure with its concrete trigger and artifacts; a screenshot alone cannot prove
audio, controller response or persistence across restart.

The new [author example](examples/query-handoff/README.md) is a small integration
reference, not a release payload or a substitute for testing downstream consumers.
Feature or source changes after validation require fresh applicable checks and
packages. Tagging, uploading and publishing remain separate explicit actions.
