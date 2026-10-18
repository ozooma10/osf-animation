# Live runtime smoke contract

Use `pwsh -NoProfile -File tools/test-smoke.ps1 -RunGame -Interactive` to build and stage the
development DLL in the bench's private output and mod directories. The bench
configures `--animation_smoke=y` only for that isolated build and restores the
production configuration afterward. Never run a normal deploying build with
instrumentation enabled. The default build excludes the runner translation unit
and all frame/teardown calls. Smoke configuration and fixtures are never release
payloads.

The bench stages `Data/OSF/animation-smoke.osf.json` from this directory and writes
`Data/SFSE/Plugins/OSFAnimationTestHarness.json` with an absolute private `stateDir`
and a fresh `launchToken`. Start Starfield with that staged mod, load a test save,
close pause menus and the console, and leave the player idle. The runner never
starts automatically. It refuses a missing player/3D, active scenes, player solo
playback.

The enabled frame pump writes `session.json` containing `pid`, decimal-string
`processStartFileTime`, `sessionId`, and the exact `launchToken`. The controller
must match all four to its launched process before submitting work. Publish an
atomic `request.json` with `id`, `sessionId`, `expiresAtUtcMs`, `command` (`start`
or `status`), and `args: { "runId": "unique-run-id" }` for start. IDs and tokens
are 1–80 ASCII letters, digits, dashes, or underscores. Requests are bounded to
64 KiB and deadlines to five minutes, checked on the actual game-thread tick.
Responses are atomic `responses/<id>.json` objects with `id`, `sessionId`, `ok`,
`error`, and `data` (the snapshot directly). Repeated adjacent request IDs do not
execute again. IDs are also remembered across intervening requests for the entire
process session, preventing an old start from replaying after a status request.
At 1,024 consumed request IDs, new commands are rejected until process restart;
old IDs are never evicted. The controller uses fresh request IDs and one
outstanding command.

Controller protocol 2 polls atomic snapshots while the game replaces its world.
The publisher uses `ReplaceFileW` for existing documents; readers permit delete
sharing and close their handles before parsing. A transient Windows sharing or
lock violation is a pending snapshot poll, while malformed JSON remains an error.
Harness regressions exercise publication with a held reader and concurrent polls.
It handles `browser-imports`, `quicksave`, and `quickload` at most once each per run,
only for the owned process/session while the controller observes an unexpired
action deadline. The controller checks expiry again after its foreground probe;
the existing input helper checks ownership and focus but does not recheck the
mailbox deadline at `SendInput`, so OS scheduling delay after dispatch remains
outside that freshness check. Browser input uses
the real document's normalized target coordinates and owned-window mouse input;
save/load use F5/F9 and therefore require the default quicksave/quickload bindings
in the disposable profile. Native observations independently prove their effects.
The controller allows 270 seconds for the runner's 240-second deadline plus IPC.
`-Hosts Released` stages the pinned UI v2.0.0 and Settings v1.0.0 ZIPs after exact
SHA-256 checks. `-Hosts Deployed` snapshots the effective hosts from the source
profile. Both disable original host providers in the private profile and retain
the selected DLL hashes. Shared overwrite host DLLs block the run.

`snapshot.json` updates after every result. It has `schemaVersion: 1`, `runId`,
`state` (`idle`, `running`, `passed`, `failed`, or `blocked`), `cases`, and
`notCovered`. Each case has `name`, `status`, and `message`; `id` and `detail` are
equivalent aliases. The authoritative required names are in `runtime-cases.json`.
Browser diagnostics retain `browserProbeAccepted` and `browserObservation`, so a
missing feed is distinguishable from an unrendered shell or rejected bridge probe.
Every case must occur exactly once with `passed` status for a runtime pass. Missing
prerequisites produce `blocked`; pending dependent cases become blocked after a
failure. A test API or graph returning success alone is insufficient: playback
clock advancement, freeze, bounded fade completion, exact callback counts, and
released actor state are asserted. The full 69-case run has a 240-second deadline.

The baseline uses the game's `cower_idle.af` without redistributing game assets.
It covers ABI/readiness, invalid handles, solo playback, speed freeze, anchor/release,
stop/fade-out, scene navigation/timers/cues/native events, and ended-roster retention.
Extended fixtures exercise generated GLTF/GLB/gzip clips, additive masks, Papyrus,
matchmaking, import records, props, equipment/input/camera/fade mechanisms, sound,
browser lifecycle, and persistence. Contract checks additionally cover malformed
native starts without side effects, busy-actor rejection, exact callback order,
bounded caller-owned roster buffers, inherited lock/apparel defaults, explicit
content opt-outs, both directions of per-start override, and exact restoration
after each policy combination. The policy fixtures are in
`animation-contracts.osf.json`. Post-load checks reject the old generational handle
and require a native callback registered before quickload to receive the full
ordered lifecycle of a newly started scene afterward.
`runtime-cases.json` lists the individual checks.
Foundation assertions additionally run query launch, manual progression, paused
node revisits, role-swapping handoff, event identities and original-state cleanup.
The alignment store is redirected only in the instrumented DLL to a file inside
the private mailbox directory, then restored on success or failure. Save/reload
assertions never modify the user's alignment file. The persistence case now saves
during active stripping/input lock/orbit and checks re-engagement plus loaded
apparel/input/camera restoration. The exact author-example JSON is staged too;
its Papyrus script is compiled but is not automatically attached to a quest.
Cleanup uses retained scene handles and releases its own registrations and mechanisms.
Every solo mutation first verifies its source clip and absence of scene
ownership; displacement blocks the run without changing the replacement. The
public solo API has no generation token, so replacing the smoke solo clip with
the identical source cannot be distinguished. Keep the test player idle.

Visual pose/culling quality and creature rigs, physical controller and full hotkey
coverage, audible mixing, visible fade/subtitle quality, weapon restoration, save
deletion and failed loads, actor 3D replacement, and full browser authoring workflows
remain separate acceptance scenarios. These gaps are emitted in every snapshot.
