# OSF Animation — real-world test plan (beta gate)

Everything verified so far is the **synthetic `OSFTest` harness** ([TESTSUITE.md](../TESTSUITE.md)).
This plan validates OSF against **real content** — actual SAF mods through the shim, community
animation packs, and authored scenes — which is the #1 risk before the beta. The SAF-migration
headline is unproven until §2 passes.

Work top-down: **§1 is the GATE** (must pass before any beta build ships); §2 is the headline; §3–6
broaden coverage. Mark each ☐ → ✅/❌ and capture the log on failure.

---

## 0. Test environment

- **Game** Starfield **1.16.244.0** (Steam) · matching **SFSE** · **Address Library for SFSE Plugins**.
- A **clean MO2 profile** (don't test in your dev profile). Note other animation/graphics mods present.
- Build under test: a **version-bumped `0.x` `releasedbg` DLL**, hash-matched to what you install.
- **Disable SAF / NAFSF** — OSF replaces them (they conflict on rig-stamping).
- Keep a known-good save **before** OSF for an A/B baseline.

### Observability
The log is the primary signal: `Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log`. Tail it live
(klogg, with the severity highlighters in [README.md](../README.md)). Useful greps:
- `event 0x` / `CUE` / `ACTION` — scene lifecycle.
- `[SAF->OSF]` / `[SAFScript->OSF]` — shimmed calls; `SHIM-GAP` — an unbridged call.
- `[W]` / `[E]` / `[C]` — warnings/errors/criticals (should be ~none in a clean run).
- `refused` / `not playable` / `failed to load` — start failures.

---

## 1. GATE smoke test (must pass — ~15 min)

| ☐ | Test | Steps | Expected |
|---|---|---|---|
| ☐ | **1.1 Loads clean** | Launch via SFSE, no save loaded | Log: `Feature report: playback hooks INSTALLED`; `Registered papyrus natives on script 'OSF'`; **no `[E]`/`[C]`** |
| ☐ | **1.2 FOMOD installs** | Install the archive 3 ways: Core-only, Core+SAF, Core+SAF+Examples | Each yields the expected files (see PACKAGING.md); game still loads |
| ☐ | **1.3 Co-load guard** | Temporarily enable SAF *and* OSF together | Startup `MessageBoxA` warning naming the conflict |
| ☐ | **1.4 Version gate** | (If feasible) run on a non-1.16.244 build | Bindings self-disable; log says playback UNAVAILABLE, no crash |
| ☐ | **1.5 One real SAF anim** | With a SAF mod installed (+ shim), trigger one of its animations on an NPC | Animation plays; `[SAF->OSF]`/`[SAFScript->OSF]` lines trace it |
| ☐ | **1.6 One real pack scene** | Install a community SLAL pack; `StartScene`/`StartSceneByTags` a 2-actor scene | Both actors play + stay aligned/anchored |
| ☐ | **1.7 Save/load survives** | Start a scene → save → load that save | Scene is gone after load; no stuck/T-posed actors; handle reads dead |

**If any 1.x fails, the beta is blocked.** Capture the log.

---

## 2. SAF migration suite (the headline)

Pick a **representative SAF-dependent mod** you actually use (an anim/pose player, a paired-scene mod).
Uninstall SAF, install OSF + the shim, keep the SAF mod. Verify by feature (coverage:
[SAF_MIGRATION.md](SAF_MIGRATION.md)):

| ☐ | Feature | What to do | Expected |
|---|---|---|---|
| ☐ | **2.1 Solo playback** | A SAF call that plays one anim (`PlayOnActor`/`PlayAnimationOnce`) | Plays; `[SAF…->OSF]` Play line |
| ☐ | **2.2 Paired scene** | A SAF 2-actor scene (`PlayScene`/`PlaySceneLocked`/`PlaySceneSeparate`) | Both actors play, co-located + synced |
| ☐ | **2.3 Sync** | `SyncGraphs`/`SyncAnimations` on a group | Frame-locked (no drift between actors) |
| ☐ | **2.4 Sequence** | `StartSequence` then `AdvanceSequence`/`SetSequencePhase` | Phases play + advance on demand |
| ☐ | **2.5 Stop** | The mod's stop/end path (`StopAnimation`) | Actors return to game control cleanly |
| ☐ | **2.6 Player lock** | A scene the **player** is in (`LockActorForAnimation*`) | Player frozen during, free after; `control lock engaged/released` in log |
| ☐ | **2.7 Speed** | `SetAnimationSpeed`/`GetAnimationSpeed` (percent) | Visible speed change; round-trips |
| ☐ | **2.8 Crosshair pick** | A crosshair-target SAF call | Returns the reticle actor |
| ☐ | **2.9 SHIM-GAP degrades** | If the mod uses a gap (absolute position, blend-graph var, selection buffer, phase callbacks) | The mod keeps running; the call is **inert + logged `SHIM-GAP`** (not a crash/hang) |

**Pass criteria:** the SAF mod is usable end-to-end; any SHIM-GAP is graceful. Note exactly which SAF
mod + version you tested (this is the evidence for the headline).

---

## 3. Native framework suite (real packs + scenes)

| ☐ | Test | Steps | Expected |
|---|---|---|---|
| ☐ | **3.1 Real pack loads** | Drop a community SLAL pack into `Data/OSF/`; `OSF.ReloadPacks()` | `PackRegistry: N animation(s) loaded`; **0 parse errors** (or named + skipped) |
| ☐ | **3.2 Tag matchmaking** | `StartSceneByTags` with the pack's tags + gendered actors | Picks a gender-fit anim; returns a handle |
| ☐ | **3.3 Multi-stage pack** | Start a pack with timed/loop stages | Auto-advances stages; ends or holds per the pack |
| ☐ | **3.4 Authored scene** | Author a real `*.scene.json` (2+ nodes, real anim ids, `advance` edges) | Walks the graph via `AdvanceScene`/`NavigateScene` |
| ☐ | **3.5 Cue + action tracks** | A node with a `cue` (numeric) + an `osf.control.lock` action | `CUE` fires at the fraction; lock engages on enter, releases on scene end |
| ☐ | **3.6 Callback round-trip** | A scripted-form receiver `RegisterSceneCallback` for `EVENT_NODE_ENTER`/`SCENE_END` | Receiver fires (reads the `OSFEvent:SceneEvent` struct). **Needs a minimal test ESP** (a quest/ObjectRef with a script that registers + logs) — note: this path is unproven without a real scripted instance. |
| ☐ | **3.7 Validation diagnostics** | A deliberately broken `*.scene.json` | Skipped + reported via `GetSceneLoadErrors()`, never fatal |

---

## 4. Stability & edge cases

| ☐ | Test | Expected |
|---|---|---|
| ☐ | **4.1 Quickload mid-scene** | F9 while a scene runs → no stuck actors, no T-pose, handle dead |
| ☐ | **4.2 Fast travel mid-scene** | Travel/cell-change during a scene → state drops cleanly |
| ☐ | **4.3 Actor unload mid-scene** | The NPC despawns/dies during its scene → no crash; scene aborts |
| ☐ | **4.4 Concurrent scenes** | Two independent scenes on different actor sets at once → both correct, independent |
| ☐ | **4.5 Exclusivity** | Start a scene on an actor already in one → refused (0), logged, original unaffected |
| ☐ | **4.6 Rapid start/stop** | Start→stop a scene ~20× quickly → no leak, no graph buildup (watch `graphCount`) |
| ☐ | **4.7 Focus-loss pause** | Alt-tab mid-scene → clock/timers freeze (expected); resume on focus — **not a bug** |
| ☐ | **4.8 No-content boot** | OSF installed, **no packs/scenes** present → loads clean, no errors |

---

## 5. Soak / performance

| ☐ | Test | Expected |
|---|---|---|
| ☐ | **5.1 Long scene** | Leave a held scene running 10+ min | No drift, no memory creep, no log spam |
| ☐ | **5.2 Many actors** | A scene with the max actor count you target | Stable framerate; alignment holds |
| ☐ | **5.3 Many packs** | 10+ real packs installed at once | Load time reasonable; id-collision warnings sane |

---

## 6. Compatibility matrix (spot-check)

| ☐ | Variable | Note |
|---|---|---|
| ☐ | Other animation mods present | (besides the one under test) — no conflict |
| ☐ | Graphics/ENB mods | the BSFadeNode near-camera fade suppression still holds (pinned actors don't vanish) |
| ☐ | Load-order positions | early vs late in the SFSE plugin order |
| ☐ | Controller + KBM | player-lock release returns control to both |

---

## Expected limitations (NOT failures)

Don't log these as bugs:
- SAF SHIM-GAPs (absolute positioning, blend-graph vars, selection buffer, phase/sequence-end
  callbacks) are intentionally inert — see SAF_MIGRATION.md.
- The game **pauses on focus loss**, so cue/timer-driven advancement freezes while alt-tabbed.
- Phase-D mechanisms are deferred/additive: free-fly/orbit camera, pool/set→clip metadata
  resolution, positioned Wwise posting, and finer equipment slot filtering. Phase-C mechanisms are
  in v0.1: `osf.equipment.*`, `osf.weapon.*`, `osf.fade.*`, `osf.voice.play`, sound, camera hold,
  settings precedence, validation, and undo-ledger cleanup.
- Scene-engine handles are **session-scoped** — dead across save/load by design.

---

## Reporting a failure

For each ❌ capture: test id · game/SFSE/OSF versions · the exact mod(s) + versions involved · the
relevant `OSF Animation.log` window (a few lines before/after) · repro steps · whether it reproduces
on a clean profile.

## Beta GATE criteria (what blocks release)

1. **All of §1** passes.
2. **§2.1–2.6** pass on **at least one real SAF mod** (the headline evidence), with SHIM-GAPs graceful.
3. **§3.1–3.3** pass on **at least one real community pack**.
4. **§4.1–4.2 + 4.5** pass (save/load + exclusivity — the safety floor).
5. No `[C]` (critical) log lines and no hard crash in any of the above.

§3.4–3.7, §5, §6 are **strongly recommended** but can be fast-followed if time-boxed — flag any
deferral on the mod page as a known beta gap.
