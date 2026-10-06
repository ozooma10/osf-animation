# Live runtime smoke contract

Configure the development DLL with `xmake f -m releasedbg --animation_smoke=y`, then
build with `xmake`. The default build excludes the runner translation unit and all
frame/teardown calls. Smoke configuration and fixtures are never release payloads.

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

`snapshot.json` updates after every result. It has `schemaVersion: 1`, `runId`,
`state` (`idle`, `running`, `passed`, `failed`, or `blocked`), `cases`, and
`notCovered`. Each case has `name`, `status`, and `message`; `id` and `detail` are
equivalent aliases. The authoritative required names are in `runtime-cases.json`.
Every case must occur exactly once with `passed` status for a runtime pass. Missing
prerequisites produce `blocked`; pending dependent cases become blocked after a
failure. A test API or graph returning success alone is insufficient: playback
clock advancement, freeze, bounded fade completion, exact callback counts, and
released actor state are asserted. The full 47-case run has a 240-second deadline.

The baseline uses the game's `cower_idle.af` without redistributing game assets.
It covers ABI/readiness, invalid handles, solo playback, speed freeze, anchor/release,
stop/fade-out, scene navigation/timers/cues/native events, and ended-roster retention.
Extended fixtures exercise generated GLTF/GLB/gzip clips, additive masks, Papyrus,
matchmaking, import records, props, equipment/input/camera/fade mechanisms, sound,
browser lifecycle, and persistence. `runtime-cases.json` lists the individual checks.
Cleanup uses retained scene handles and releases its own registrations and mechanisms.
Every solo mutation first verifies its source clip and absence of scene
ownership; displacement blocks the run without changing the replacement. The
public solo API has no generation token, so replacing the smoke solo clip with
the identical source cannot be distinguished. Keep the test player idle.

Visual pose/culling quality and creature rigs, physical controller and full hotkey
coverage, audible mixing, visible fade/subtitle quality, weapon restoration, save
deletion and failed loads, actor 3D replacement, and full browser authoring workflows
remain separate acceptance scenarios. These gaps are emitted in every snapshot.
