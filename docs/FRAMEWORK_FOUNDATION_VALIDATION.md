# Framework foundation implementation and validation

Implementation completed on 2026-10-15, source revision `c96d474` on `dev`.
Sections 0 through 4 of [the design](FRAMEWORK_FOUNDATION_DESIGN.md) are implemented.
The API contract and usage details are in [API.md](API.md).

| Slice | Commit |
| --- | --- |
| Frozen anchors and actor-owned baselines | `f0024eb` |
| Persistent speed and per-scene pause | `11bc294` |
| Event identity and termination cause | `bb73f32` |
| Compatible engine API minor negotiation | `efd6375` |
| Configurable scene keys and alignment capability | `e029433` |
| Atomic alignment persistence | `261ed17` |
| Live alignment, HUD and APIs | `9ed2a81` |
| Retained anchor reference/base | `ab5b718` |
| Input grant revisions and explicit revocation | `eebbf60` |
| Same-cast handoff, role rebinding and incremental equipment ownership | `c96d474` |

## Automated verification

- Both DLLs built in `releasedbg`, with `animation_smoke=n`.
- All **12 native suites passed**, including new cases for frozen placement, reordered baselines,
  speed/pause, API negotiation, alignment parse/round-trip/pruning/revisit, input grant expiry,
  role permutations/eligibility, incremental equipment restore order, and native event payloads.
- All four Papyrus sources compiled successfully with the CK compiler.
- Browser verification passed **162 tests in 8 files**, TypeScript checking and the Vite build.
- SDK lock verified all four vendored files against the released UI/Settings tags.
- Browser implementation boundary check and Git whitespace checks passed.

The final native build and all native suites ran again from an exported `c96d474` source tree,
including its pinned CommonLibSF and commonlib-shared dependencies. XMake used explicit `-P .`
in `build/foundation-source-c96d474`. This excludes the separate uncommitted pose-blending edits
in the main checkout. Browser UI sources matched this revision. The original staged/unstaged
design document and unrelated edits were preserved.

The verified snapshot was deployed through XMake to the configured MO2 Engine and Browser mods.
**13 deployed files matched SHA-256**, including both DLLs, fresh PEX files, source scripts and
schemas. The local evidence manifest is `build/foundation-deployment-c96d474.json`.

`npm ci` reported 11 dependency advisories (1 moderate, 3 high, 7 critical) in the existing lockfile.
The Browser verification succeeded; dependency upgrades were outside this implementation.

## In-game acceptance still required

These automated checks do not establish the design's live behavior/visual acceptance:

- Five-stage drift check, furniture anchor restoration, and each actor's own restore position after a role swap.
- Visible half-speed playback and independent pause/resume across two live scenes.
- Papyrus callback causes, compatibility with an old compiled consumer, and silent world replacement.
- Live alignment save/restart, authored-offset reload, unsaved revisits, and disabled alignment controls.
- Handoff pose blend/accepted position snap, apparel and control continuity, authored exit-action precedence,
  locked/disabled replacement input, furniture rejection/offset recomposition, and failed-playback cleanup.
- Connection and existing calls from the actual released 1.5.0 Browser DLL to the newer engine. The
  legacy 3.0 prefix and negotiation predicates are tested offline; this binary combination needs a game run.

No release or live-game acceptance is claimed by this record.
