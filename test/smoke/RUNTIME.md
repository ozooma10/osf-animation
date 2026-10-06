# Live runtime smoke contract

Configure the development DLL with `xmake f -m releasedbg --animation_smoke=y`, then
build with `xmake`. The default build excludes the runner translation unit and all
frame/teardown calls. Smoke configuration and fixtures are never release payloads.

The bench stages `Data/OSF/animation-smoke.osf.json` from this directory and writes
`Data/SFSE/Plugins/OSFAnimationTestHarness.json` with an absolute private `stateDir`
and a fresh `launchToken`. Start Starfield with that staged mod, load a test save,
close pause menus and the console, and leave the player idle. The runner never
starts automatically. It refuses a missing player/3D, active scenes, player solo
playback, or an existing player overlay.

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
released actor/route state are asserted. The run has a 45-second deadline.

This baseline uses the game's `cower_idle.af` without redistributing game assets.
It covers ABI/readiness and invalid handles, AF solo playback, speed freeze,
same-position anchor/release, stop/fade-out, registered scene start/stage/graph
navigation/timer/cues/native events, ended-roster retention, and zero-animation
overlay owner/admission/requests/cleanup. Scene fixture policies disable apparel,
held-item, camera, input and fade mechanisms and follow the current actor position.
Cleanup uses retained scene/route handles and releases its own callback/owner;
world replacement abandons actor mutations and blocks the run.
Every solo mutation first verifies its source clip and absence of scene/route
ownership; displacement blocks the run without changing the replacement. The
public solo API has no generation token, so replacing the smoke solo clip with
the identical source cannot be distinguished. Keep the test player idle.

This is a baseline smoke pass, not proof of all engine behavior. Visual pose and
culling quality, multi-actor synchronization, GLTF playback in game, props and
equipment/weapon restoration, camera/input locks, fade, audible Wwise output,
Papyrus delivery/rebinding, save/load lifecycle, animated overlay handoffs and 3D
suspension and browser input remain separate acceptance scenarios. These
gaps are emitted in every snapshot so automation cannot silently claim them.
