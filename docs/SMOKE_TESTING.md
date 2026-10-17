# Automated release testing

Run from the repository with PowerShell 7.2 or newer:

```powershell
pwsh -NoProfile -File tools/test-release.ps1 -Plan
pwsh -NoProfile -File tools/test-release.ps1 -Interactive
# Explicitly omit in-game checks (not a complete release run):
pwsh -NoProfile -File tools/test-release.ps1 -Offline
# Use currently deployed host DLLs instead of the pinned releases:
pwsh -NoProfile -File tools/test-release.ps1 -Interactive -Hosts Deployed
```

The release command includes the game by default and requires explicit `-Interactive`
permission for keyboard, mouse and focus control. `-Plan` stays read-only and
`-Offline` requires no interactive permission. It shares the smoke bench's
implementation, evidence validation, lock and exit codes. VS Code exposes it as
**Release: Full automated suite** and **Release: Offline checks**.
The existing smoke commands remain available:

```powershell
pwsh -NoProfile -File tools/test-smoke.ps1 -Plan
pwsh -NoProfile -File tools/test-smoke.ps1
pwsh -NoProfile -File tools/test-smoke.ps1 -RunGame -Interactive
# Test the UI / Settings DLLs currently deployed in MO2:
pwsh -NoProfile -File tools/test-smoke.ps1 -RunGame -Interactive -Hosts Deployed
```

The same commands are available as **Smoke: Offline** and **Smoke: Offline + Game** in
VS Code. This follows the OSF Settings bench pattern: one entrypoint, independent
failure reporting, retained evidence, and an explicit distinction between offline
tests and in-game acceptance. It is an on-demand bench, not a scheduled job.

## Prerequisites

- XMake, the C++23/MSVC toolchain and the repository dependencies/submodules.
- Node/npm compatible with `extensions/browser/ui/package.json`. The bench runs `npm ci`
  before the browser scripts.
- Compiled `dist/Scripts/OSF{,Types,Advanced}.pex` for packaging. Compile them with the
  canonical command in `AGENTS.md` after Papyrus edits. Packaging rejects older PEX
  files and mismatched compiled native signatures. The CK compiler is also required
  for the private `OSFAnimationSmoke.pex` fixture; the live suite calls shipped OSF
  natives and receives `OSFTypes:SceneEvent` structs through that compiled script.
- For `-RunGame`: the sibling **OSF Test Harness** with `Test-AnimationRelease.ps1`,
  prebuilt input/native helpers, installed Starfield/SFSE/dependencies, and its
  configured disposable seed save. Use `-Harness <directory>` if it is elsewhere.
  The harness owns the local game session and checks process identity, its native
  handshake, desktop/foreground state, and private save/profile ownership. An
  unrelated running game or missing fixture blocks the run. The disposable seed
  needs a loaded human player, movement enabled, no active scene, and at
  least one worn vanilla armor item. A temporary second actor is created and
  deleted. The suite also quicksaves and quickloads inside the private save folder
  using the default F5/F9 bindings. Keep Starfield foreground and leave input idle
  until the run completes; losing foreground can block input/capture acceptance.
- Runtime integration defaults to the published **OSF Settings v1.0.0** and **OSF UI v2.0.0**
  ZIPs from GitHub. Downloads are cached by their published SHA-256, verified, and
  installed in private test mods. Missing releases/checksums or network access block
  this gate. `-Hosts Deployed` instead copies the configured MO2 `OSF Settings` and
  `OSF UI` mods into the private test mods and records their DLL hashes. This tests
  your latest deployed host changes without changing the vendored released SDKs.
  It does not build the sibling projects or silently substitute their source trees.

No ordinary development mod is overwritten by the bench. The production build
installs under this run's evidence directory. The runtime DLL is built in
`build/smoke-native` and staged separately, leaving `build/windows/x64/releasedbg`
available to the release packager. The original deployment environment variables
are restored, and the final XMake configuration is `releasedbg`, `build`, with
`animation_smoke=n`.

## What runs

| Stage | Evidence |
| --- | --- |
| Coverage | Every registered XMake suite and runtime case is mapped in `test/smoke/coverage.json`. Inventory drift fails. |
| Bench contracts | Rejects missing suites, stale or changed evidence, mismatched source/DLL hashes, skipped game cases, and truthy string flags. Exercises release receipts, instrumented DLL rejection, staged/ZIP tampering, forbidden entries and Papyrus mismatch rejection. |
| SDK | `tools/Test-SdkLock.ps1`: vendored OSF UI / OSF Settings SDK files match `sdk-lock.json` hashes; with sibling checkouts present, the locked tags and commits also match. |
| Production | Normal releasedbg DLL, installed into private staging. |
| Native | All twelve `xmake test` suites, including synthetic AF/rig/BA2 import, playback stage/mark semantics, production catalog conversion, clip-duration cache separation, shared scene-launch behavior and browser/engine boundary coverage. Executed suite identities and counts must match the inventory. |
| Fixtures | Compiles the private Papyrus script; generates the live GLTF/GLB/gzip clips and WAV; decodes and samples the three clips through the production importer. |
| Browser | Independent production addon DLL build, clean dependency install, all Vitest assertions, TypeScript check, and production Vite build. A fresh JSON receipt must include every test file, with no skipped/todo tests; individual assertions appear in the aggregate report and JUnit. |
| Distribution | Engine and browser FOMOD ZIPs; required entries, exact production DLL hash, separation of payloads, and no test fixtures, source maps, or player values. |
| Install matrix | Fresh sandbox installs of engine only, minimal browser, browser + library, browser + emotes, and everything. Each installs twice, verifies exact payload hashes and preserves synthetic user content/cache/settings and another mod's schema. This tests file overlays, not MO2 or gameplay migration. |
| Runtime preflight | Harness protocol version, host modes, save/load/browser input controller and exact case inventory agree before building the instrumented DLL. An outdated harness blocks with a corrective message. |
| Runtime build | Optional, separate instrumented DLL and live fixture content. |
| Runtime | Optional, owned disposable game session, per-case assertions and retained native snapshots/screenshots. |
| Final checks | Production configuration restored; source fingerprint unchanged. |

The native importer suite generates its own small assets. It exercises real
GLTF/GLB/gzip decoding and ozz sampling without copyrighted game assets or a
developer's local clip library. Existing standalone `osf-import-test` and
`osf-af-import-test` remain useful for specific content.

The game suite currently has **58 assertions** across the 18 areas in the coverage
manifest. It uses installed vanilla assets plus generated test content:

- Native API validation; compiled Papyrus calls, scene callbacks and post-load rebind.
- AF, GLTF, GLB and gzip playback; live rig binding/stamping, resolved additive mask
  bones/weights; solo speed, observed anchor displacement/restoration and stop;
  shared clock/speed across two live actors.
- Scene stages, navigation, timers, events, explicit roles, files scenes, pause/resume
  and cancellation; invalid starts, actor admission, bounded roster queries, exact
  event ordering; tagged matchmaking and per-file registry diagnostics.
- Actual input masks and worn inventory changing/restoring; native orbit camera
  state restoring; live prop attachment/reattachment/destruction; bounded fade and
  subtitle deadlines; sound-pool lookup and Wwise voice replacement/stop.
- Real quicksave/quickload, fresh `.sfs`/`.osf` evidence, persistence callbacks,
  saved-value restoration, FormID resolution and active scene teardown on load.
- Scene-start default lock/strip policy, opt-outs, explicit native API overrides,
  exact restoration after each variant, stale handles and retained callbacks after load.
- Settings value/issue APIs; real browser document and catalog/library/import/wheel
  feeds, including the shipped library larger than the host's per-message limit;
  decoded library counts rendered by the application; a trusted Windows click
  opening Imports; browser close/reopen and wheel display.

The browser observer is injected only into the private staged document. It reports
DOM state and trusted input; it does not synthesize page clicks or replace the host
bridge. Native lifecycle/engine observations and retained captures remain separate
from pixel quality or audio acceptance. Missing prerequisites block the run;
failed observations fail it. Independent feature groups continue after owned cleanup;
shared-prerequisite failures and unsafe cleanup stop the run. Unexecuted cases are
reported as blocked. A command acknowledgement alone cannot complete the suite.

## Results and automation

Each invocation creates a new `build/smoke/<UTC timestamp>-<id>` directory containing:

- `report.md`, `result.json`, and `junit.xml`, including each executed native suite,
  browser assertion, every live assertion and
  per-feature completion; offline live cases are explicitly not run;
- per-stage logs, source identity including dirty/untracked files and submodule
  contents, and production/runtime DLL hashes;
- both archives, their SHA-256 hashes, manifests tied to production build receipts,
  browser JSON results and the five disposable installer layouts;
- with `-RunGame`, a runtime suite receipt pointing to the harness's captures,
  session, requests/responses and final native snapshot.

Use `-OutputDirectory <new directory>` to choose another destination. Within this
repository it must be under `build/smoke` so output cannot alter its own source
fingerprint. Existing directories are rejected. Bench invocations are serialized
with an exclusive file lock; ordinary editing/build commands should stay idle
while it runs. `-Jobs 4` controls native compilation parallelism.

Exit codes are **0** for the requested checks passing, **1** for a failed assertion
or build, and **2** for blocked infrastructure or missing evidence. Skipped stages
remain skipped in JUnit. An offline success is `offline-passed` with
`completeAutomatedSuite=false`. A complete game run can report
`automated-passed-acceptance-gaps-open`; this means every implemented automatic
case passed, not that every engine feature has been proven.

## Coverage boundaries

The coverage manifest lists every major subsystem and its remaining gaps. Passing
all 58 cases is **not** proof of every feature, asset or failure path. Manual or
additional fixture acceptance remains for visual sync/pose/culling/prop fit,
creature rigs, audible mixing/spatialization, visible fade/subtitle quality,
physical controllers and the full launcher/hotkey matrix, weapon draw/sheath,
engine race/keyword filters, actor 3D unload/reload, every termination cause,
delete/failed-load/restart paths, and complete wheel persistence/browser editing.

To extend the bench, add assertions in `src/Testing/AnimationSmoke.cpp` or
`AnimationFeatures.inl`, add content under `test/smoke`, then update the compiled
`AnimationCases.h` inventory, `runtime-cases.json` and coverage mapping together.
The bench rejects inventory drift. Instrumentation is compiled only with
`--animation_smoke=y`. The owning mod handles a small session/deadline-checked
mailbox on the normal frame-maintenance thread; no testing command or fixture is
included in the default release build.

An interrupted or failed run keeps its evidence. Use the harness's normal owned
session stop command if a process remains; never close an unrelated game to make
a smoke run pass.

## Browser acceptance after updating the hosts

Use matching engine/browser builds with the released Settings/UI hosts and record
each result independently:

- Core-only installation with Browser and OSF UI absent: gameplay consumers and playback work, `OSF.OpenBrowser()` returns false,
  and Mod Issues contains no warning merely because the optional browser is absent.
- Browser installed with missing UI: engine playback remains available and the addon is inactive without an issue.
- Browser installed with unsupported UI or invalid assets: engine playback remains available;
  addon-owned Mod Issues gives the corrective reason and reinstall/update action.
- Open through Settings' Launcher and bound keyboard/controller hotkeys; use Back
  inside the view before closing it. Confirm game input and cursor return on close.
- Start a player scene and an NPC-only scene in separate runs. Confirm player
  scenes stop on close, NPC-only scenes survive and resume their prior speed,
  orbit releases, and reopening gives fresh tokens and state.
- Reload the document, close while it is loading, reopen quickly, and load another
  save while open. Confirm stale play/selection/orbit commands do not affect the new
  presentation or world. Retest failed/cancelled loads and focus loss separately.

Keep visual/controller and save-load observations alongside the automated report;
passing the smoke suite alone does not close these acceptance gates.
