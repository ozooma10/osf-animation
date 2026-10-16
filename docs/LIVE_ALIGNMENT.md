# Live scene alignment

The authoring tool adjusts each participant's position and heading during anchored playback.
The shared animation clock keeps running; speed zero pauses both actors for inspection. Offsets
are temporary until saved into a scene file. This edits whole-actor placement; hand/elbow contact
still requires an animation edit.

## Enable and connect

Install the freshly built engine and compiled `OSFAdvanced.pex`, then start a fresh game process.
Enable the file interface once in the console:

```text
cgf "OSFAdvanced.SetAlignmentToolsEnabled" true
```

Close the console. Files live in the actual Documents location, including redirected Documents:
`My Games/Starfield/OSF/Alignment`. The interface starts disabled on every game launch. Disable it
with the same console command and `false`. Disabling closes the interface; use `reset` first to undo
live offsets, or end the scene normally. Teardown retains the existing pre-scene transform restore.

For unattended authoring, launch the game with the process environment variable
`OSF_ANIMATION_ALIGNMENT=1`. With MO2, close its existing instance first and set this variable only
in the process launching MO2 so the game inherits it. This is an explicit alternative to the console
opt-in, not a persisted engine setting. Ordinary launches without it keep the bridge disabled.

Python 3.10+ is required, with no third-party packages. From the repository root:

```powershell
python tools/alignment/alignment.py status
python tools/alignment/alignment.py attach --scene mypack.hug --scene-file 'C:\MyMod\OSF\hug.osf.json'
python tools/alignment/alignment.py pause
python tools/alignment/alignment.py nudge --role partner --y 0.01
python tools/alignment/alignment.py nudge --role partner --heading 1
python tools/alignment/alignment.py set --role partner --x 0 --y 0.03 --z 0 --heading 0
python tools/alignment/alignment.py speed 0.25
python tools/alignment/alignment.py resume
python tools/alignment/alignment.py reset
```

The enabled bridge can also launch registered content with an explicit ordered cast, stop the
attached scene, reload content after stopping all scenes, and set the active scene-orbit camera:

```powershell
python tools/alignment/alignment.py start --scene osf.prototype.sleepintimate.embrace.qa --actors 0x5986 0x14 --anchor 0x14
python tools/alignment/alignment.py attach --scene osf.prototype.sleepintimate.embrace.qa --scene-file 'C:\MyMod\OSF\embrace-qa.osf.json'
python tools/alignment/alignment.py camera --azimuth 90 --elevation 10 --radius 2.5 --height 1.1
python tools/alignment/alignment.py stop
python tools/alignment/alignment.py reload
```

Start requires loaded, distinct actor references, refuses participants already playing, uses a
participant as anchor, keeps clothing visible and selects anchored placement plus scene orbit.
It uses the normal registered-scene launcher and its role/admission checks.
After `stop`, allow the normal animation blend-out to finish before starting the same cast again
(about one second for the default blend). An immediate start can correctly report actors still playing.
Start dismisses the character and pause menus; other menus must be closed normally. Camera angles are
absolute world degrees; radius and height above the scene floor are metres. Camera and stop use
the same exact playback/segment identity as offsets. Camera requires a single active scene and
an already active orbit; it cannot acquire a camera hold. There is no arbitrary console execution.

`status` lists active scenes, runtime identities, roles, clips, time, speed, authored placements,
temporary offsets, and effective placements. `attach` requires exactly one active instance of the
requested scene ID. A new instance, stage change/restart, save load, or bridge restart needs a fresh attach.

Translation is in **metres along the scene anchor's axes**; heading is in **degrees relative to that
anchor**. `nudge` adds to the selected role's temporary offset; `set` replaces specified components.
Other components/participants retain their values. Both add to the authored placement. Heading
rotates around the clip origin, which can differ from the pelvis when root displacement is baked in.
Bounds: +/-5 m and +/-180 degrees per adjustment; authoring speed 0..4. `reset` clears all offsets
without changing speed. `resume` sets speed to 1.

## Save accepted alignment

Attach with `--scene-file` pointing to the **physical source**, for example inside an MO2 mod. The
game's virtual `sourceFile` in `status` helps identification but may not be the editable source.

```powershell
python tools/alignment/alignment.py save
```

The client checks scene identity, clip order/resources, original placements, and the source hash.
It creates a uniquely named backup beside the source, then atomically writes **effective** offsets
into the current stage's clip entries. Existing clip metadata and other scenes/stages remain intact.
Per-clip placement overrides inherited role/template offsets, preventing double application.
Repeated saves are idempotent. An external source edit causes a refusal, not an overwrite.

Saving leaves the current pose alone. Stop the scene, run `python tools/alignment/alignment.py reload`,
then start and attach again to use the saved data. The console alternative is `cgf "OSF.ReloadPacks"`.
Temporary adjustments expire with their original segment entry.

Direct linear `stages[].clips`, single `clip`, and direct `nodes[].clips` are supported. For reused
`use` nodes, generated/ad-hoc scenes, or a separate reviewable artifact, export the snapshot:

```powershell
python tools/alignment/alignment.py export 'C:\Work\hug-alignment.json'
```

## File interface

Filesystem polling, parsing, and responses run on a worker at 100 ms intervals. Game operations use
`NativeMainThreadQueue`. One command is outstanding at a time. The client serializes whole
read/modify/write operations between CLI processes.

- `ready.json`: schema, enabled state, bridge session token, PID, UTC heartbeat.
- `command.json`: atomically replaced request, maximum 64 KiB.
- `response.json`: acknowledgment with matching session/sequence, `ok`, and `scenes` or `error`.
- `client-state.json`: client-only selection/source hash, never read by the engine.

Request shape (replace the illustrative session/target/sequence/expiry with current values):

```json
{
  "schema": 1,
  "session": "<ready.json session>",
  "sequence": 1,
  "expiresAtUtcMs": 0,
  "op": "set",
  "target": { "handle": 1, "playback": 2, "revision": 3 },
  "offsets": [
    { "x": 0, "y": -0.01, "z": 0, "heading": 0 },
    { "x": 0, "y": 0.01, "z": 0, "heading": 0 }
  ],
  "speed": 0
}
```

Sequence increases within a bridge session. Expiry must be in the future, at most 60 seconds away,
and is checked again on the game thread. The client uses its request timeout. Target comes verbatim
from `status`; revision identifies a segment **entry**, including same-index restarts. Supply every
offset in participant order. `set` permits speed only or offsets only. `reset` takes a target and no
mutation fields. `status` takes neither. Invalid fields, nonfinite values, stale identities, and
duplicate/out-of-order sequences are rejected before mutation. Acknowledgment confirms live state
was updated; rendering and the guarded capsule update follow on game frames.

The engine never writes authored files; source verification, backups, and saving belong to the
client. Native placement uses existing manager/graph locks. Authored stage data stays immutable.
There are no public native ABI layout changes.

## Verification

```powershell
xmake test
python -m unittest discover -s tools/alignment -p test_alignment.py -v
```

Live check: start a looping pair, attach, pause, record the snapshot, nudge one role 1 cm, and confirm
its effective position changes while shared time stays fixed. Reset, verify the original placement,
then resume. End/restart and verify the old attachment is refused. Check saving against a disposable
source copy. Offline checks cover protocol, segment identity, coordinates, transport, and guarded
saving; visible movement and capsule behavior require the game check.
