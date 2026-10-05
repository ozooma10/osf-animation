# Automated smoke bench

Run from the repository with PowerShell 7.2 or newer:

```powershell
pwsh -NoProfile -File tools/test-smoke.ps1 -Plan
pwsh -NoProfile -File tools/test-smoke.ps1
pwsh -NoProfile -File tools/test-smoke.ps1 -RunGame
```

The same commands are available as **Smoke: Offline** and **Smoke: Offline + Game** in
VS Code. This follows the OSF Settings bench pattern: one entrypoint, independent
failure reporting, retained evidence, and an explicit distinction between offline
tests and in-game acceptance. It is an on-demand bench, not a scheduled job.

## Prerequisites

- XMake, the C++23/MSVC toolchain and the repository dependencies/submodules.
- Node/npm compatible with `ui/animation-browser/package.json`. The bench runs `npm ci`
  before the browser scripts.
- Compiled `dist/Scripts/OSF{,Types,Advanced}.pex` for packaging. Compile them with the
  canonical command in `AGENTS.md` after Papyrus edits. Packaging rejects older PEX
  files and mismatched compiled native signatures. Native runtime assertions do
  **not** validate the Papyrus VM bindings or struct layout.
- For `-RunGame`: the sibling **OSF Test Harness** with `Test-AnimationRelease.ps1`,
  prebuilt input/native helpers, installed Starfield/SFSE/dependencies, and its
  configured disposable seed save. Use `-Harness <directory>` if it is elsewhere.
  The harness owns the local game session and checks process identity, its native
  handshake, desktop/foreground state, and private save/profile ownership. An
  unrelated running game or missing fixture blocks the run.
- Runtime integration uses the published **OSF Settings v1.0.0** and **OSF UI v2.0.0**
  ZIPs from GitHub. Downloads are cached by their published SHA-256, verified, and
  installed in private test mods. Missing releases/checksums or network access block
  this gate; sibling development DLLs cannot substitute for these released hosts.

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
| Native | All eight `xmake test` suites. The reported execution count must match the inventory. |
| Browser | Clean dependency install, Vitest, TypeScript check, and production Vite build. |
| Distribution | Engine and browser FOMOD ZIPs; required entries, exact production DLL hash, separation of payloads, and no test fixtures, source maps, or player values. |
| Runtime build | Optional, separate instrumented DLL and live fixture content. |
| Runtime | Optional, owned disposable game session, per-case assertions and retained native snapshots/screenshots. |
| Final checks | Production configuration restored; source fingerprint unchanged. |

The native importer suite generates its own small assets. It exercises real
GLTF/GLB/gzip decoding and ozz sampling without copyrighted game assets or a
developer's local clip library. Existing standalone `osf-import-test` and
`osf-af-import-test` remain useful for specific content.

The game suite uses the installed vanilla human `cower_idle.af`; its JSON fixtures
contain asset references only. It tests API readiness/invalid handles, solo play,
speed, anchor calls, asynchronous stop, scene stages/navigation/timer events and
cleanup, and zero-animation overlay owner/station cleanup. It refuses to start
over existing player playback, waits for fade-out completion, and retains case
results instead of inferring success from a command acknowledgment.

Integration cases read Animation's Settings value, report and clear a transient
`EXPECTED TEST` issue, verify the released UI API and browser registrations, and
open/host-close/reopen/close the browser through native lifecycle callbacks. This
proves the logical presentation callbacks reached Animation's frame queue; it does
not prove document rendering or physical input. Engine-free queue tests cover
hidden commands, duplicate notifications, stale work after close/reopen, and world
replacement invalidation.

## Results and automation

Each invocation creates a new `build/smoke/<UTC timestamp>-<id>` directory containing:

- `report.md`, `result.json`, and `junit.xml`;
- per-stage logs, source identity including dirty/untracked files and submodule
  contents, and production/runtime DLL hashes;
- both archives, their SHA-256 hashes, and manifests tied to production build receipts;
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

The coverage manifest lists every major subsystem and its remaining gaps. It is
part of the report, rather than an informal claim of full coverage. In particular,
this first runtime suite does not establish multi-actor visual sync, creature rigs,
physical controller input, camera/fade/apparel restoration, audible Wwise output,
prop visibility, Papyrus callbacks, engine form matchmaking, save/load/delete
hooks, real browser/WebView2 behavior, or Studio Link. The route-specific matrix
remains in [OVERLAY_ACCEPTANCE.md](OVERLAY_ACCEPTANCE.md).

To extend the bench, add assertions in `src/Testing/AnimationSmoke.cpp`, add any
content under `test/smoke/data`, then update `test/smoke/runtime-cases.json` and the
coverage mapping together. Instrumentation is compiled only with
`--animation_smoke=y`. The owning mod handles a small session/deadline-checked
mailbox on the normal frame-maintenance thread; no testing command or fixture is
included in the default release build.

An interrupted or failed run keeps its evidence. Use the harness's normal owned
session stop command if a process remains; never close an unrelated game to make
a smoke run pass.

## Browser acceptance after updating the hosts

Use matching engine/browser builds with the released Settings/UI hosts and record
each result independently:

- Core-only installation: playback works, browser launch reports the missing addon,
  and Mod Issues contains no warning merely because the optional browser is absent.
- Browser installed with missing/unsupported UI or invalid assets: engine playback
  remains available; Mod Issues gives the corrective reason and reinstall/update action.
- Open through Settings' Launcher and bound keyboard/controller hotkeys; use Back
  inside the view before closing it. Confirm game input and cursor return on close.
- Start a player scene, NPC-only scene and preview in separate runs. Confirm player
  scenes/previews stop on close, NPC-only scenes survive and resume their prior speed,
  orbit releases, and reopening gives fresh tokens and state.
- Reload the document, close while it is loading, reopen quickly, and load another
  save while open. Confirm stale play/selection/orbit commands do not affect the new
  presentation or world. Retest failed/cancelled loads and focus loss separately.

Keep visual/controller and save-load observations alongside the automated report;
passing the smoke suite alone does not close these acceptance gates.
