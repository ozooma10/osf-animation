# OSF Animation 2.0 candidate verification

The 2026-10-22 candidate implements V2_SCOPE A-C and updates the Browser, Intimacy, Seduce,
SDK examples and workspace harness callers. It is unpublished. The original scope and layer
specifications remain local, untracked and unchanged.

## Migration checkpoint checks

| Check | Result |
| --- | --- |
| Animation production build | Engine and Browser DLLs built with `xmake build -j 1`, exit 0 |
| Animation native suites | `xmake test`: 16/16 passed, exit 0 |
| Browser view | 166 tests passed; TypeScript and Vite build passed |
| Animation private probe build | `animation_smoke=y`, separate `build/v2-smoke`; both DLLs built, exit 0 |
| Intimacy production and private probe builds | Both configurations built, exit 0 |
| Intimacy native suites | 8/8 passed, exit 0; includes audio ownership and API contracts |
| Intimacy content suites | 4/4 passed, exit 0; includes semantic resolution, candidates and active host guards |
| Papyrus | Animation 4, Intimacy public 2, private fixtures 10, Seduce fixtures 9, Seduce 11 and query/handoff example 1 compiled |
| Seduce content | Static content checks passed; conversion retained 8 semantic bindings, 106 beats and 154 voice assets |
| Authoring fixtures | Sidecar schema validation passed; changed PowerShell files parse |
| Generators | D3: 7 nodes; highlights: 161 nodes; unique IDs and resolved routes; changed Python generators parse |

Builds cleared `XSE_SF_MODS_PATH` and `XSE_SF_GAME_PATH` to disable deployment. Private probe
options were restored off, with production output under `build`. Compiler and suite logs are
retained locally in `%TEMP%/osf-v2-01a164fd`. Earlier compilation failures exposed stale API
callers and were repaired before the passing runs recorded here.

## Offline release gate, 2026-10-22

`tools/test-release.ps1 -Offline -Jobs 1` passed with exit code 0 against commit `a98ffb0`.
The retained run is `build/smoke/v2-offline-20261022-01`: `result.json` reports `offline-passed`
and `completeAutomatedSuite=false`. All eleven required offline stages passed, including:

- 16 native suites, 166 Browser tests, typecheck/build, private Papyrus fixtures and importer checks;
- 98 smoke-bench contract checks, 28 release-validation checks, SDK locks and Browser isolation;
- engine and Browser 2.0.0 archives, compiled Papyrus signatures, source/build receipts and hashes;
- all five disposable FOMOD combinations installed twice, preserving synthetic user and other-mod files;
- restoration of production configuration and an unchanged source fingerprint during the run.

The Browser build helper now receives the gate's job limit; it no longer silently uses four
compiler jobs. No game or MO2 action ran. Each later candidate needs its own fresh report and
matching archive hashes; this dated result does not qualify subsequent source changes.

## Contract coverage

- The depth-one composer delegates to `PoseMath`. Its fixture uses byte comparisons for
  override/additive parity, nonzero base pads and scale, masked inheritance and declined writes.
  Stacks deeper than one are refused without output writes. `Graph.cpp`, `StampPose` and live-base
  capture are unchanged; no runtime layer hookup or multi-layer playback is included.
- Registry tests cover flat nodes, actual segment indices, inherited defaults, retired-schema
  rejection, cross-file routes and immutable publication references. Route validation runs after
  generated/provider nodes are merged; provider addition/removal can change route availability
  without reparsing authored files, while pinned older publications remain valid.
- Scene ABI 2.0 and services 4.0 reject the previous majors. Authored edges and exact requests
  share admission/preparation; cover is a boundary requirement. Per-start policy overrides,
  ownership, cancellation and generation checks remain in that path.
- Intimacy events use global nodes; `Change` now denotes every committed global-node transition.
  Audio voice and contact scopes close at that boundary for both authored and requested changes.
  Contact ownership survives state refreshes/internal segments within one node. Pure tests and
  private harness assertions encode those boundaries.

## Unverified acceptance

No game was launched and nothing was deployed to MO2. The private probes were compiled, not
executed. Fresh live tests are still needed for transition blends and covered reveals, pose/base
capture, apparel/input/camera restoration, audio timing and audible continuity, controller/input,
save/load, hot reload, browser interaction and creature rigs. Existing 1.x runtime evidence does
not qualify the 2.0 candidate.

Release packaging and the disposable install matrix require a fresh
`tools/test-release.ps1 -Offline -Jobs 1` report with outcome `offline-passed`. The report under
`build/smoke` binds both archives to the exact source fingerprint and binary hashes and records
each installer combination. The dated gate result above supplies that evidence for its recorded source. Actual MO2
installation/upgrade, binary distribution and third-party consumer acceptance remain separate. Old alignment files are preserved but refused by the v2 reader; users must review
and reauthor global-node alignment entries. Multi-layer runtime playback remains deferred.
