# OSF Animation 2.0 candidate verification

The unpublished 2.0 candidate implements V2_SCOPE A-C and updates the Browser,
SDK examples and workspace harness callers. Installed consumer migration remains incomplete,
as recorded below. The scope/layer documents are tracked design records;
multi-layer runtime implementation (D) remains deferred. The last published release is 1.4.0;
the 1.5 development baseline was superseded before publication.

## Full release gate, 2026-11-12

`build/smoke/v2-release-live-20261112-02/result.json` reports
`automated-passed-acceptance-gaps-open`, `completeAutomatedSuite=true` and
`automatedPassed=true`. All 69 live assertions, 16 native suites, 169 Browser tests,
58 Papyrus native signatures, both archive checks and the repeated installer matrix passed.
The game evidence is retained in the sibling Test Harness at
`artifacts/20261112-150835-967-AnimationSmoke`.

The run used `-Hosts Isolated`: released UI v2.0.0 plus the private Settings host from
`OSF Intimacy/build/smoke/desktop-ready-20261020/candidate`. Released Settings v1.0.0
does not honor the private values-directory override. This host choice protects normal
saved values, but does **not** qualify the exact released Settings DLL. The foundation
scenario uses separate instrumented Animation DLLs; consumer checks use production DLLs.
The report binds source revision `dda9722` plus its recorded dirty-source fingerprint.
This documentation entry was added after that run; it does not rewrite its receipt.

| Verified production artifact | SHA-256 |
| --- | --- |
| Animation DLL | `3928A8686085A1DD50E36EAB05CD860B88142F81CD1F9B5D04D3325EB3C4F836` |
| Animation 2.0.0 ZIP | `9A3908E6D3D88CD4F6C88596ECD29C5632679004CD15EA3393E934B6C224DD83` |
| Browser 2.0.0 ZIP | `7D602A2581785FDA4A24D31E206450B5E820BA4A93EF312AF5A0355DB1CB4236` |

The release entrypoint now forwards an explicit isolated Settings candidate to the smoke
runner. Foundation staging disables other OSF native consumer modules in its copied
profile so its module-identity check measures the intended combination. The corresponding
103 smoke-bench contract checks and 47 harness coordinator checks passed. The first
attempt, `v2-release-live-20261112-01`, was interrupted before game launch to correct
Settings isolation; its partial evidence is retained and is not a pass.

## AISS, Seduce and Autonomous compatibility, 2026-11-12

This focused follow-up supersedes the Seduce/Intimacy blockers and AISS version-specific
findings in the November 11 audit below. It does not resolve other consumers' findings.
The installed Animation DLL still matches the production payload from
`build/smoke/v2-compat14-offline-20261111-01`; no production engine changes were needed.

| Consumer | Offline result and changes |
| --- | --- |
| AISS 4.01 | All 265 PEX files decoded; inspected OSF-family calls and SceneOptions members match installed contracts. All seven legacy scene files load through the production adapter: 121 nodes, no errors or warnings. All 250 distinct referenced clips were found in installed loose files or BA2 name tables. AISS was not modified. |
| OSF Seduce | Migrated to schema-2 global nodes and `EncounterRequest.NodeId`, then recompiled all 11 scripts against current Intimacy. All 12 nodes load without errors; the three original entry IDs, authored clips, timing, cues and routes are preserved. Entry-only tags keep random starts out of interior nodes. Eight semantic bindings now address global nodes and pass the current Intimacy schema. |
| OSF Intimacy | Rebuilt current source for scene ABI 2.0/services ABI 4.0 with fresh public PEX and production instrumentation disabled. Deployed the complete 21-file production payload together with matching Seduce scripts/content. The old installed DLL requested unsupported unpublished 1.5/3.5 interfaces. |
| OSF Autonomous NPC Interactions 1.2 | Inspected calls and SceneOptions/SceneEvent members match installed contracts. Its legacy scene loads as one node with four sound cues and no errors; its clip is installed. Added a generated current Settings schema, preserving all 27 keys/defaults while converting groups and enum labels. Original scripts/assets and the retired-path schema are unchanged; old saved settings are not migrated. |

Validation passed: Animation's 16 native suites and 58 Papyrus signature checks,
Intimacy's eight native suites and four content/host suites, Seduce's 11-script compile,
and content/schema checks. The Seduce conversion checker also retained all 106 native
beats and 154 existing voice assets. Final installed hashes match the tested Intimacy
production payload, all 11 Seduce PEX files, 187 Seduce content/source files and the
generated Autonomous settings schema. This is a focused compatibility verification,
not a new full release gate or live qualification.

Evidence: `build/consumer-check-20261112` contains decoded consumers, production-parser
reports, asset inventory and pre-deployment backups of Intimacy and Seduce. The final
Intimacy candidate/receipt is in the sibling repository at
`build/consumer-compat-20261112-final`. The reusable Autonomous conversion lives in
the sibling `OSF Compatibility Packs/autonomous-settings` directory. The scene registry
test executable now accepts `--inspect <root containing Data/OSF>` to report imported
consumer definitions; it explicitly does not validate asset availability. Asset inventory
across installed mods does not prove profile activation or successful clip decoding.

The offline migration above launched no game or changed a profile. At that checkpoint
MO2 selected `Default`, where AISS, Seduce and Autonomous were all disabled. The later
live follow-up qualifies only the cases and combinations explicitly recorded below.
The original in-game acceptance inventory was:

- AISS: exercise pair/solo triggers, delayed and failed admission, fallback, timed advance,
  stop and redressing. A reserved positive handle does not yet mean playback has begun.
- Seduce/Intimacy: test dialogue and explicit/random entry, Bridge/DownDog navigation,
  voices and callbacks, interruption, equipment/control restoration and save/load. Verify
  configured actor anatomy/access and the existing Haters body-fit requirement; no actor
  setup was silently changed. The old Stage debug helper selects internal segments, not
  global nodes; use Intimacy navigation for node changes.
- Autonomous: confirm the Settings page and setting changes, scene begin/cue/end/node-exit
  handling, initial speed and cleanup. Exercise delayed admission against its IsPlaying
  desynchronization audit, plus interruption and save/load.
- Run these checks with required animation providers enabled, including a pre-upgrade save
  to cover existing Papyrus state. Offline name/arity/member checks do not establish full
  VM compatibility or saved-state migration.

### Copied-profile live follow-up

The final run, `build/consumer-check-20261112/live/runs/20261112-153845-455-Animation-Consumers`,
finished `passed`, with no evidence warnings. All **54 consumer assertions** passed, plus
idle/active save completion and cross-load controls/apparel comparisons. Its copied Default
profile enabled the three consumers, their required animation assets, and the same isolated
Settings host described above. The loaded production Animation hash matches the release
artifact. Production Intimacy was
`10C76CBD998385D9175662434A8504B409CD9BBF6031BA0795F15315F1A0C97C`.
The current Intimacy candidate receipt is `OSF Intimacy/build/consumer-live-20261112-02`.

| Path exercised in the actual installed/rebuilt scripts | Observed result |
| --- | --- |
| Unchanged AISS `AISSStartSceneById` helper | Solo and pair playback, legacy linear advance to the next global node, stop, invalid-ID refusal, controls and apparel restoration; 13 checks |
| Rebuilt Seduce / production Intimacy | Bridge and DownDog start, original entry IDs, encounter ownership, Bridge navigation to `/build`, stop and restoration; 15 checks |
| Unchanged Autonomous quest manager | Bound quest, current Settings writes, solo/pair start, speed 1 after BEGIN, active-scene audit and emergency cleanup; 12 checks |
| Active Seduce plus world replacement | 8 active-start checks, completed quicksave with sidecar, that exact sidecar parsed with five blocks/five loads and no callback failures, 6 post-load checks, controls/apparel equal to the pre-scene baseline |

These invoke the real consumer entrypoints; they do not drive AISS's AI conversation or
Seduce's dialogue UI, or wait for Autonomous's natural scheduling/eligibility scan. Autonomous
requires furniture by default, so its pair test explicitly disabled that requirement only in
private Settings values for the empty fixture cell. Original consumer PEX hashes were checked;
compile-only declarations were never staged over those scripts. The combined registry loaded
136 nodes from ten files with zero problems.

Save testing established two UI preconditions: dismiss AISS's startup text-entry prompt, and
hide the Intimacy encounter panel before requesting a save. Idle quicksave stalled while the
AISS prompt was open; active quicksave still stalled with the encounter panel open after that
prompt was dismissed. Hiding the panel without ending the encounter allowed the active save
and reload to complete. This is **not** a pass for saving while either UI remains open. Failed
attempts and the interrupted evidence-writer run remain beside the passing run. No native
Animation or Intimacy production fix was needed for the exercised paths.

Test preparation now recognizes the installed male-skeleton folder alias and uses the preserved
Seduce entry IDs instead of retired `/intro` names throughout affected Intimacy fixtures and
expected callback sequences. All ten private smoke scripts, nine production fixtures, two
public Intimacy scripts and eleven Seduce scripts compiled; changed PowerShell files parse.
The rebuilt production Intimacy DLL is byte-identical to the deployed candidate.
After cleanup, all 16 recorded Default-profile/Settings files matched their original hashes,
no consumer test profile remained, and the owned game had exited.

Before release, still perform hands-on acceptance on the intended shipping host combination:

- Trigger AISS through actual conversation, Seduce through dialogue/random selection, and
  Autonomous through its normal scheduler with furniture, location/privacy filters and NPCs.
- Check delayed/failed admission, natural completion, combat/unload interruption, repeated use,
  and an existing pre-upgrade save across a full game restart.
- Inspect animation alignment, blends, multi-actor sync and body/gear fit; listen to voices,
  spatial audio and subtitles. Exercise first-person/zoom restoration and focus loss.
- Use Browser/Settings/encounter controls with keyboard and a physical controller, close/reopen
  panels, and verify saving after Hide/Escape. Exact released Settings acceptance remains open.
- Install/upgrade the final archives through MO2, checking settings, pins, alignment migration
  and hotkey rebinding. Other consumers in the November 11 audit are outside this focused pass.

## Installed consumer audit, 2026-11-11

Read-only inspection covered 141 DLLs, 524 PEX files (including private test duplicates),
and eight mod BA2 archives. All 522 loose installed-mod PEX files decoded without parser errors;
the two archived PEX files received string inspection. Native request versions were verified
from installed machine code. Papyrus checks covered function names, argument counts and accessed
struct members, not full type checking, saved VM migration or runtime behavior.

| Consumer | Installed compatibility and remaining work |
| --- | --- |
| OSF Intimacy | Blocked: requests scene 1.5/services 3.5. Current source has 2.0/4.0 headers; build and deploy a matching complete package, then qualify it live. |
| Suit Protocol | Animation integration blocked: requests scene 1.3/services 3.0. Current source has 2.0/4.0 headers; update the installed build and test equipment changes/interruption. |
| OSF Seduce | Blocked through Intimacy. Installed PEX and current source still write `EncounterRequest.SceneId`; current Intimacy source exposes `NodeId`. Migrate/recompile Seduce together with Intimacy, including matching scripts/types/content. |
| AISS | All 28 inspected OSF calls match names/arity; used struct members exist. Test delayed/failed admission, fallback, timed advance, stopping and redressing. |
| OSF Defeat | Scene ABI 1.1 and its 128-byte options prefix are supported. Installed code immediately stops then starts a scene; test pending teardown/admission, repeat defeat, recovery and load. |
| OSF Companions Prototype | Bindings match, but its timer drops the scene handle if `IsPlaying` is false after 0.25 seconds. Retain/cancel pending ownership explicitly and test delayed admission. |
| Animation Browser | Installed scene 2.0/services 4.0 match; offline checks pass. Live qualification remains open. |

Outpost Siege uses unchanged persistence ABI 1.0, not the animation scene API. Body Physics
detects Animation's presence but does not request its scene/services APIs; visual coexistence
remains untested. Snu Snu/Gergel packs are content-only. The original SAF Seduce uses SAF,
not OSF. No additional normal Animation trigger consumers were identified in this scan.

MO2 selected `OSF Testing`, with normal gameplay consumers disabled; `Default` enables
Intimacy and Suit Protocol. No profiles or mods were changed and no game was launched.
Raw local evidence is retained under `build/consumer-audit` (ignored build output).
These findings supersede earlier claims that all sibling consumers were already migrated.

## Published 1.4 compatibility, 2026-11-11

The [compatibility adapter](COMPATIBILITY_1.4.md) changes native/Papyrus code after the
initial documentation-only preparation below. Both production DLLs and fresh PEX are
built/deployed for this change; no game run, commit or publication is implied.
The candidate gate for this revision is `build/smoke/v2-compat14-offline-20261111-01`.
Read its generated `result.json`, `report.md` and archive hashes for actual status;
this reference is not itself a passing receipt. Previous evidence does not qualify it.

New offline coverage includes the frozen published vtable/options prefixes, five
unchanged v1.4 JSON fixtures, graph/linear/use conversion, role inference/templates,
legacy stage launch rules and pinned reload references. All 16 native suites pass in
the focused run. The live compatibility checklist in the linked contract remains open,
especially unchanged consumer PEX/SceneOptions saves and a real ABI-1.2 consumer DLL.
Preparation remains asynchronous for legacy clients too.

## Initial release preparation, 2026-11-11

Current scope: reconcile the 2.0 documentation and run the offline release gate. This step does
not launch the game, deploy to ordinary MO2 mods, publish archives or qualify runtime behavior.
The repaired candidate's authoritative outcome, source fingerprint, case counts and archive hashes
belong in `build/smoke/v2-offline-20261111-02/result.json`, `report.md` and `archive-hashes.json`.
Read those generated files before treating the run as passed; this planned evidence path is not
itself a success receipt. Failed attempts remain retained and retries use a new directory.

```powershell
pwsh -NoProfile -File tools/test-release.ps1 -Offline -Jobs 1 -ForceVerification `
  -OutputDirectory build/smoke/v2-offline-20261111-02
```

The gate builds production engine/Browser DLLs, runs native/Browser/fixture checks, verifies both
archives and the repeated installer matrix, restores production configuration, and checks that
source did not change during execution. Do not edit source or docs while it runs. Reports qualify
only their exact source and binary identities; subsequent changes require fresh evidence.

The initial `v2-offline-20261111-01` run failed its release-validator self-test and packaging:
the local October 20 `OSF.pex` still exported `GetSceneId` instead of the source's `GetNode`.
Its 16 native suites, 169 Browser tests, typecheck/build, SDK checks and private fixtures passed;
the installer matrix could not run without verified archives. The failure report is retained.
All four public/development Papyrus scripts were then recompiled with the canonical CK command;
`tools/verify-papyrus.py` confirmed all 58 native signatures match source and registrations.
No PSC or native source change was required. `OSFTest.pex` remains excluded from release archives.

A separate `npm audit` is retained in the first run's `npm-audit.json`: 15 affected packages
(3 moderate, 5 high, 7 critical, including transitive ratings). Browser verification passing does
not resolve these advisories. Dependency exposure/update review remains a release follow-up;
no dependency upgrade or claim of shipped-runtime exploitability is made by this preparation.
A November 12 recheck still reports the same 15 affected packages; its raw report is
`build/consumer-check-20261112/npm-audit-current.json`. The live passes do not resolve it.

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
  Stacks deeper than one are refused without output writes. The layer-composer change does not
  alter runtime stamping or live-base capture; no runtime layer hookup or multi-layer playback
  is included. Other playback fixes are separate changes.
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

## Current-candidate live gates

The October 22 migration/offline checkpoint launched no game. Later October 22-23 live runs did
execute, as summarized below and in the sibling qualification record. Those passes qualify their
recorded binaries, not the later merged candidate, role-emitter change or Settings split.

| Gate | Required evidence on the intended release combination |
| --- | --- |
| Foundation and consumers | November 12's 69-case Animation suite and 54 focused consumer checks passed on the combinations recorded above. Broader migrated Intimacy/Seduce continuation, role swaps, manual/automatic progression and refusal/interruption paths still need current-combination acceptance. |
| Camera and input restoration | First-person and nonzero third-person zoom baselines through stop, completion, interruption, handoff and load. Exercise pause/manual combinations, focus loss and auto-vanity separately; retain any reproduction of the unexplained historical failure. |
| Transition/pose quality | Visually inspect seamless and covered changes, the half-turn rotation fix, spine/mask seams, additive base capture, rig/3D rebuilds, multi-actor sync, furniture anchoring and representative creature rigs. |
| Role-emitter audio | Listen to near/far and moving actors, compare listener/role emission, test actor unload and world replacement, cancellation/late decode, ordinary completion tails, mixing and subtitles. Earlier automated audio runs predate the role-emitter change. |
| Browser, Settings and hosts | Use the actual release host versions: reopen menus/panels and Browser, reload catalogs while visible/hidden, exercise launcher/wheel/keyboard and physical controller, and verify cursor/input return. Check separate alignment-page persistence/rebinding and fixed speed aliases. The panel-reopening UI fix was only qualified as a private payload. |
| Ownership and recovery | Save during active locked/equipped scenes, load and restart, verify equipment/input/camera cleanup, conflicting equipment/weapon demands, props, provider withdrawal, hot reload, failed/cancelled load and save-delete paths. |
| Install and upgrade | Install both candidate archives through the real mod manager in a copied 1.4 profile; migrate custom content/alignment, rebind hotkeys, check pins/preferences through restart. Exercise core-only and missing/incompatible optional-host combinations. |

Use disposable profiles/saves and retain host versions, source receipts, loaded module hashes,
logs and observations. Physical controller, human listening and visual review cannot be closed by
offline tests. Automated game success still leaves the [coverage boundaries](SMOKE_TESTING.md#coverage-boundaries).

Release packaging and the disposable install matrix require a fresh
`tools/test-release.ps1 -Offline -Jobs 1` report with outcome `offline-passed`. The report under
`build/smoke` binds both archives to the exact source fingerprint and binary hashes and records
each installer combination. The dated gate result above supplies that evidence for its recorded source. Actual MO2
installation/upgrade, binary distribution and third-party consumer acceptance remain separate. Old alignment files are preserved but refused by the v2 reader; users must review
and reauthor global-node alignment entries. Multi-layer runtime playback remains deferred.

## Live camera regression, 2026-10-22

The disposable Intimacy qualification reproduced third-person zoom loss with a
stable wheel-selected baseline of 1.0: scene free-camera entry changed it to 0.0,
and cleanup restored camera mode 20 but left zoom at 0.0. The failure is retained
as `q3-h4-zoom-01` / `20261023-010355-849-H4-Active` in the
[Intimacy execution record](../../OSF%20Intimacy/docs/ANIMATION_2_LIVE_QUALIFICATION.md).
The measured gameplay interval contained only the recorded setup wheel input;
previous unrelated injected events occurred before gameplay.

Commit `f08840c` captures current and target third-person zoom with the shared
camera baseline, restores them after final mode handback, and drops that baseline
on world replacement. It does not change Graph, StampPose or layer composition.
The one-job private preparation gate in `build/smoke/v2-prepared-20261023-01`
passed 16 native suites, 166 Browser assertions, both package checks and all five
repeated installer combinations. It produced these DLL identities:

| Build | SHA-256 |
| --- | --- |
| Production engine | `9D5986F4E10C46473B0CAB60982A67C145D931F7E9DD69989BE743D4C9FF80DE` |
| Instrumented engine | `4A227D7D48D395155C8BE391C136C7A6B112966A80DC4ECE8A3041134CAD9948` |

That preparation preceded the fix commit; its source receipt records the exact
pre-commit revision and files. The foundation runner includes HEAD and documentation
in its source identity, so a fresh post-commit preparation is required before its
next full runtime gate. Never rewrite the earlier receipt to imply it was built
from a later checkout.

## Subsequent live checkpoint, 2026-10-23

The [Intimacy execution record](../../OSF%20Intimacy/docs/ANIMATION_2_LIVE_QUALIFICATION.md)
records the later results; its chronological entries supersede earlier pending notes:

- The zoom fix passed all 12 nonzero-zoom H4 cases. Q1's 69 assertions, both Q2 interruption
  matrices, Q3 ownership/save/revalidation and automated Q4 checks passed on recorded pairs.
- Q5 fallback and policy cases passed. A separate reopening failure was fixed in OSF UI commit
  `f22ee80f`; the focused live WebView2 test passed on a private UI payload. The broader matrix
  and live CEF acceptance were not repeated with that payload.
- Applicable Q6 automated audio measurements passed. Human listening and physical controller
  review were unavailable; these runs do not qualify the later Animation role-emitter change.
- An Intimacy continuation defect was fixed in `fb6388d`. All seven selected Q7 production runs
  passed on Animation `9D5986F4...` / Intimacy `B5893594...` (1,046 assertions including repeated
  preflights). Older consumer results do not blanket-qualify that newer Intimacy binary.
- `first-person-targeted-01` passed 12 camera cases / 276 assertions with zero injected input,
  observing mode 0 before and after each scene and mode 15 during playback. It used the earlier
  Animation production DLL above with instrumented Intimacy, not the later merged Animation
  binary or a full production pair. No camera code changed in that follow-up.

The historical intermittent first-person failure remains unexplained; the clean repetition did
not establish its cause or qualify auto-vanity. Q8 and full live release acceptance remain open.
The targeted follow-ups preserved normal mods/profiles/saves and stopped at that checkpoint.
