# Global loop count tests

`LoopGlobalsTest.cpp` exercises parsing, fallback, capture-at-start, node timing,
and playback completion in the engine-free scene registry suite (`xmake test`).

For the focused in-game check, build the production DLL normally, close Starfield
and idle MO2, then run from this repository:

```powershell
pwsh -NoProfile -File test/loop-globals/Run-Game.ps1
```

The runner uses the sibling OSF Test Harness background entrypoint and its owned
`OSF Testing` profile/disposable save. It stages the production DLL, a generated
one-second clip, a private GLOB plugin, and a compiled Papyrus probe only in
`OSF Testing Output`. Existing files are never overwritten. The probe calls the
public Papyrus API and records a repeating midpoint cue plus the scene-end cause.
Assertions require the exact cue count and natural completion for every case:

| Case | Expected loops |
| --- | ---: |
| Global is 2, changed to 4 immediately after start | 2 |
| Next scene reads the changed global | 4 |
| Graph node reads global 2 | 2 |
| Global 2 with LoopScale 2 | 4 |
| Missing plugin, default 3 | 3 |
| Missing form in the loaded test plugin, default 3 | 3 |
| Global is negative, default 3 | 3 |

Evidence is retained in `build/loop-globals-game/<run-id>`: staged payload hashes,
command receipt, Papyrus event log, and `result.json`. The harness keeps its own
launch/ownership evidence. Cleanup stops only the owned game, removes the staged
files, and removes the test plugin from the private profile. Production mod files
and shared saves are not modified. No interactive input or smoke DLL is required.

## Pause and inventory regression

`Run-Game.ps1 -MenuPauses` uses the same private profile and cleanup with an isolated
`animation_smoke=y` DLL at `build/smoke-native/windows/x64/releasedbg`. Build it using
the separate smoke output and a private `XSE_SF_MODS_PATH`, then restore production
configuration (`releasedbg`, output `build`, `animation_smoke=n`). Never deploy this
DLL to the normal mod or package it.

The probe starts counted and timed scenes through Papyrus with the player-input
lock enabled. Background mode posts real PauseMenu and DataMenu messages on the
game thread. With explicit permission, add `-Interactive` to use the game's normal
Escape/Tab/I keyboard entry paths and include InventoryMenu; this focuses the owned
game. Inventory requires its normal entry context, so a bare menu-show message is
not used. Both modes observe open state and pause requests, check the clock stays
frozen for four seconds, then verify the original scene/playback survives and
advances after closing each menu and outlasting the watchdog's resume grace.
`pause-result.json`, `pause-observations.jsonl`, and `animation.log` retain the four
background or six interactive cases and native evidence.
