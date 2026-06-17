# OSF Animation — E2E Test Suite (lean core)

*The core's behavioral test suite. In-game behavior can't be CI'd, so this document is the
regression suite: run the GATE subset before any release, and the full suite after any game
patch / CLSF bump / hook change. Every case has an exact pass signal — visual, or a line in
`Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log`.*

**Status legend:** ✅ PASS (date) · ❌ FAIL · ⬜ NOT RUN · 🚫 BLOCKED/DEFERRED (reason)
**GATE** = must pass before first public release (LAUNCH.md Phase 1).

> **Statuses marked "(pre-split)" were validated on the pre-carve full framework.** The lean
> core is a verbatim migration of that engine code, so those behaviors are expected to hold —
> but a fresh in-game pass on the `OSF Animation` build is the Phase 1 GATE. The scene runtime
> (graphs, cues, the `osf.control.lock` action, callbacks) merged in and is tested separately; the
> remaining policy mechanisms (equipment/fade/voice/sound/camera) land via Phase C; specific adult
> content is the **OSF Seduce** mod.

## How to run

1. Build the tree under test: `xmake` (auto-installs DLL + pex + packs to the MO2 mod).
2. MO2: enable `OSF Animation` → launch via SFSE. The `solo`/`pair`/`test.stages`/`test.loops`
   baked-in entries need no animation mod.
3. Console basics: click an NPC for its refID; `cgf` calls take the forms shown.
   `OSFTest.psc` wraps the array-taking natives.
4. **Fixture saves** (make once, keep): `F-NPCs` — near two expendable NPCs;
   `F-MidScene` — hard save made while a scene (`OSFTest.Loops`) is playing on two NPCs;
   `F-Endgame` — a save that can reach the Unity (for SAVE-05, optional).
5. Record results in the Run Log at the bottom.

---

## Suite BOOT — startup, bindings

| ID | Test | Steps | Expected | Status |
|---|---|---|---|---|
| BOOT-01 | Clean boot report | Launch; read log top | Game-version line; one-line feature report with `playback hooks INSTALLED`; `registered SaveLoadEvent begin sink`; `registered TESLoadGameEvent backstop sink` | ⬜ **GATE** (lean core) |
| BOOT-02 | Version native | `cgf "OSF.GetVersion"` | semver string printed | ⬜ **GATE** |
| BOOT-03 | Pack scan | Read log after BOOT-01 | OSFTestPack defs loaded, no parse errors; `cgf "OSFTest.Reload"` rescans cleanly | ✅ 2026-06-12 (pre-split) |
| BOOT-04 | Readiness handshake | `cgf "OSFTest.Ready"` | HUD: ready true; scenes/sync/anchor all true | ⬜ **GATE** |
| BOOT-05 | Co-install warning | Enable NAFSF or SAF DLL for one boot | Warning in log (+ blocking message box); disable after | ✅ 2026-06-14 (pre-split) |

## Suite SOLO — single-actor playback

| ID | Test | Steps | Expected | Status |
|---|---|---|---|---|
| SOLO-01 | Play/stop + blend | `cgf "OSF.Play" player "OSF\Animations\OSF_Test\StandSurrender01.glb"` then `cgf "OSF.Stop" player` | Pose blends in from idle (~0.4 s), plays, blends back out on stop | ✅ 2026-06-12 (pre-split) |
| SOLO-02 | Baked-in solo (no animation mod) | `cgf "OSFTest.DefinedSolo" "solo" <ref>` | Plays shipped StandSurrender01; proves the release zip is self-sufficient | ✅ 2026-06-12 (pre-split) |
| SOLO-03 | Speed / freeze | During SOLO-01: `cgf "OSFTest.Speed" <ref> 0.0` then `0.5` then `1.0` | Freezes, half speed, authored speed | ✅ 2026-06-12 (pre-split) |
| SOLO-04 | Anchor / unanchor | `cgf "OSFTest.Anchor" <ref> <x> <y> <z> <hdeg>`; then `cgf "OSFTest.Unanchor" <ref>` | Rendered pose pins to the world point + heading; unanchor returns it to following the actor | ✅ 2026-06-12 (pre-split) |
| SOLO-05 | Sequence | `cgf "OSFTest.Sequence" <ref>` | 2-phase sequence loops the whole thing (phase x2, phase x2, repeat) | ✅ 2026-06-12 (pre-split) |
| SOLO-06 | Stop refuses scene members | Start a scene; `cgf "OSF.Stop" <participant>` | Refused with log message; scene unaffected | ✅ 2026-06-12 (pre-split) |

## Suite SCENE — multi-actor scenes, stages, placement

| ID | Test | Steps | Expected | Status |
|---|---|---|---|---|
| SCENE-01 | 2-actor sync | `cgf "OSFTest.Pair" <refA> <refB>` (baked-in `pair`) | Both actors aligned at anchor, frame-locked; `cgf "OSFTest.StopPair" <refA>` ends both | ✅ 2026-06-12 (pre-split) |
| SCENE-02 | Placement math | `cgf "OSFTest.PairApart" <refA> <refB>` | B at +1 m Y, heading 180° relative to anchor (a visibly "wrong" but stable pose) | ✅ 2026-06-12 (pre-split) |
| SCENE-03 | Timed auto-advance | `cgf "OSFTest.Stages" <refA> <refB>` (baked-in `test.stages`) | Stages advance on each stage timer; scene auto-ends after the last stage (log: `Stopped scene`) | ✅ 2026-06-12 (pre-split) |
| SCENE-04 | Start at stage / manual jump | `cgf "OSFTest.DefinedStage" "test.stages" 2 <refA> <refB>`; another run `cgf "OSFTest.Stage" <refA> 1` | Starts at stage 2; manual jump swaps stage with blend | ✅ 2026-06-12 (pre-split) |
| SCENE-05 | **Loop-count advance** | `cgf "OSFTest.Loops" <refA> <refB>` (baked-in `test.loops`); watch the log | Stages advance after the loop target (not a timer): `loop target expired — auto-advanced to stage 2/2`, then `final stage loop target expired`, then `Stopped scene` | ⬜ **GATE** |
| SCENE-06 | Hold-forever | `cgf "OSFTest.Defined" "pair" <refA> <refB>` (timer/loops ≤ 0) | Holds indefinitely; manual stop ends it | ✅ 2026-06-12 (pre-split) |
| SCENE-07 | Tag matchmaking | `cgf "OSFTest.PlayTag" "paired" <refA> <refB>` | HUD shows the chosen id; a matching def plays | ✅ 2026-06-12 (pre-split) |
| SCENE-08 | Ad-hoc files | `cgf "OSFTest.StartPair" "pair" ...` / `StartSceneFiles` via a consumer | Co-locates at actor[0], anchors, syncs, plays | ✅ 2026-06-12 (pre-split) |
| SCENE-09 | Getters | During a scene: `GetSceneStage`, `IsPlaying` | Correct stage; -1 when not in a scene; IsPlaying true through fades | ✅ 2026-06-12 (pre-split) |

## Suite SHIM — SAF compatibility (the launch headline)

| ID | Test | Steps | Expected | Status |
|---|---|---|---|---|
| SHIM-01 | Ping → IsReady | `cgf "SAFScript.Ping"` | true once OSF is up | ⬜ **GATE** |
| SHIM-02 | PlaySceneSeparate | A SAF consumer (or `cgf "SAFScript.PlayScene" <a> <b> "<anim1>" "<anim2>"`) | Two actors co-located, anchored, synced — same as StartSceneFiles | ⬜ **GATE** |
| SHIM-03 | StopAnimation | `SAF.StopAnimation` on a solo and on a scene participant | Solo → Stop (fade); participant → StopScene | ⬜ **GATE** |
| SHIM-04 | SyncAnimations / SyncGraphs | Play two solos, then the shim's sync | Frame-locked on one clock (no promote-to-policy) | ⬜ **GATE** |
| SHIM-05 | Existing SAF mod | Install a real SAF-dependent mod, run its scene | Plays unchanged through the shim | ⬜ **GATE** |

## Suite PLAYER — standalone compat locks

The core never auto-applies player locks; these test the `OSFCompat` mechanism the SAF shim uses.

| ID | Test | Steps | Expected | Status |
|---|---|---|---|---|
| PLAYER-01 | Control + camera lock | `cgf "OSFCompat.SetPlayerControlLock" 1` + `cgf "OSFCompat.SetPlayerCameraLock" 1`; then both `0` | Movement/jump/etc. disabled + forced third person while held; restored on release | ⬜ **GATE** |
| PLAYER-02 | Scroll-zoom alive / FP bounce | While locked: scroll wheel; try zooming into first person | Scroll-zoom works (POVSwitch not blocked); first person bounces back to third | ⬜ **GATE** |
| PLAYER-03 | AI-driven cleared on load | Lock, hard-save, reload | After load the persistent AI-driven flag is cleared (player controllable; `StopAll` released it) | ⬜ **GATE** |

## Suite SAVE — save-safety, load matrix

Static ground truth (SaveSafety.cpp `IsWorldReplacingLoadOp`, osf-re `engine.save_load`):
world-replacing loads = exactly {kLoadMostRecent, kQuickload, kLoad, kLoadNamedFile}; **new game
and Unity/NG+ fire no SaveLoadEvent** — the TESLoadGameEvent backstop owns them. Each: start a
scene (`cgf "OSFTest.Loops"`) on two NPCs, trigger the load, grep the log.
`StopAll (<reason>)` self-identifies which sink fired. The backstop also re-binds the natives.

| ID | Test | Steps | Expected | Status |
|---|---|---|---|---|
| SAVE-01 | Quickload | F9 mid-scene | `StopAll (save-load begin)`; no crash; clean world after load | ⬜ **GATE** |
| SAVE-02 | Load by name | Menu → Load mid-scene | `StopAll (save-load begin)` | ⬜ **GATE** |
| SAVE-03 | **Death reload** | Mid-scene `player.kill` → auto-reload | `StopAll (save-load begin)` — the kLoadMostRecent path | ⬜ **GATE** |
| SAVE-04 | New game | Mid-scene → main menu → New Game | NO `save-load begin`; backstop only: `StopAll (save loaded (TESLoadGameEvent backstop))` | ⬜ **GATE** |
| SAVE-05 | Unity/NG+ | Mid-scene → enter Unity (needs F-Endgame) | Same as SAVE-04 | 🚫 defer if no endgame save (statically proven same path as SAVE-04) |
| SAVE-06 | Native re-bind after load | After any load above: `cgf "OSF.GetVersion"` / start a scene | Natives still bound (no "Unbound native function"); scene plays — proves the VM re-bind | ⬜ **GATE** |
s
After every SAVE row also confirm: no actor left frozen, no stuck pose pinned to a dead anchor.

---

## Release gate summary

All **GATE** rows green = LAUNCH.md Phase 1 done. Efficient order (2 short sessions):

1. **Boot + solo/scene session** (fixture F-NPCs): BOOT-01/02/04; SOLO-01; SCENE-05 (watch the
   loop-advance log); SCENE-09. One extra boot for BOOT-05. Make the F-MidScene save here.
2. **Shim + player + load-matrix session:** SHIM-01…05; PLAYER-01/02/03; SAVE-01 → 02 → 03 → 04
   (→ 05 if F-Endgame) → 06.

## Run log

| Date | Game ver | Plugin ver / commit | Scope | Result | Notes |
|---|---|---|---|---|---|
| 2026-06-12 | 1.16.244 | pre-split WT | solo, scenes, anchor, sync, sequence, save-safety | PASS | validated on the pre-carve full framework (migrated verbatim) |
|  |  |  |  |  |  |

*After a run: update Status cells + this log; if a GATE row fails, it blocks release — fix and
re-run the row, not the whole suite.*
