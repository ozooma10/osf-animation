# Profiling OSF Animation

Two complementary tools:

| Tool | Answers | Binary |
| --- | --- | --- |
| **Profiler override** (in-house) | Which OSF systems cost what, per frame and per event, on every thread: named scopes, lock waits, allocations, counters, frame cost; a Perfetto timeline | A separate development-only DLL deployed as an MO2 override |
| **WPR/WPA CPU sampling** | Where CPU time goes at function level, including inside the engine and third-party code | The ordinary shipping DLL, unmodified |

## The shipping DLL carries no instrumentation

Every profiling macro compiles to nothing (or to the plain statement it wraps) unless the build is
configured with `--profiling=y`. The production configuration is `--profiling=n` (the default), and
the profiler core (`shared/Profiling/*.cpp`) and engine host (`src/Profiling/*.cpp`) are not compiled
into it at all.

Release safety:

- Every profiling build embeds the marker `OSFAnimationProfilingBuild`. `Test-InstrumentedDll`
  (`tools/ReleaseRules.ps1`) rejects it, so the packager, the smoke bench and the game scripts refuse
  to ship or stage it.
- Production build receipts record `profiling=n`. Every scripted `xmake f` passes `--profiling=n`.
- `osf-profiler-test` compiles `test/ProfilerNoopTest.cpp` without the define and checks that the
  production expansion has no side effects and that the lock macro is an ordinary lock.

When the instrumentation was added (2026-10-22), the production DLL was compared with one built from
the previous commit: identical `.text` size and identical function set (7370). The only code
differences were source-line constants in log calls whose lines moved, branch targets in one
function that moved, and one register-allocation change caused by such a line constant. Repeat the
comparison when you add instrumentation with anything other than the macros below.

## Build and enable the profiler override

With Starfield closed, from this repository:

    pwsh -NoProfile -File tools/profiling/Build-ProfilingOverride.ps1

The script configures `build/profile` with `--profiling=y`, builds only the engine target, deploys
`OSF Animation.dll`, its PDB and a `meta.ini` to `<mods>\OSF Animation Profiling`, checks that the
normal mod's DLL did not change, and restores the production xmake configuration even on failure.
The override contains nothing else; it inherits scripts, schemas and content from the normal mod.

In MO2, enable **OSF Animation Profiling** at a higher priority (lower in the left pane) than
**OSF Animation**; the Conflicts tab must show it winning `SFSE/Plugins/OSF Animation.dll`. Disable
it after profiling. The normal mod is never touched, so no rebuild is needed to go back.

The SFSE log confirms the override:

    [Profile] OSFAnimationProfilingBuild: development-only profiler build of OSF Animation. Never ship or package this DLL.
    [Profile] capture started (stats) at load; commands go to ...\SFSE\OSF\profiles\command.json

## Capture modes

| Mode | Records | Cost per scope* |
| --- | --- | --- |
| `off` | Frame times only: the overhead baseline | about 1 ns (one relaxed load) |
| `stats` | Per thread and scope: count, total, min, max, log2 histogram, allocations; OSF time per frame; counters and gauges | about 45 ns |
| `trace` | `stats` plus a per-thread event ring (default 131072 events, 4 MB per thread) for a timeline | about 45 ns |

\* Measured by `osf-profiler-test` (it prints the numbers) on the development machine. Two
`QueryPerformanceCounter` reads dominate.

The override starts in `stats` at plugin load, so the first dump includes boot and registry loading.

## Commands

`tools/profiling/Invoke-Profiler.ps1` writes `command.json` into the profiles folder and waits for
the game to acknowledge it in `status.json`. The game reads commands about once a second from its
frame hook. **Starfield pauses while unfocused**, so switch to the game after issuing a command, or
pass `-Focus`.

    # Fresh timeline capture of a two-actor scene
    pwsh -NoProfile -File tools/profiling/Invoke-Profiler.ps1 -Command start -Mode trace -Label two-actor -Focus
    # ...play the scene...
    pwsh -NoProfile -File tools/profiling/Invoke-Profiler.ps1 -Command mark -Label climax -Focus
    pwsh -NoProfile -File tools/profiling/Invoke-Profiler.ps1 -Command dump -Show -Focus

| Command | Effect |
| --- | --- |
| `start [-Mode stats\|trace]` | Reset the window and record in that mode. `-RingEvents` (1024..4194304) sizes new trace rings. |
| `stop` | Stop recording (frame times continue). Data stays until `start` or `reset`. |
| `dump [-Reset]` | Write the dump files; `-Reset` starts a new window afterwards. `-Show` prints the stats. |
| `mark -Label x` | A labelled instant on the trace timeline. |
| `reset` | New window, same mode. |
| `status` | Report only. |

`-AutoDump on|off` applies with any command. **Automatic dumps** are on by default: at every scene
end (`scene-end-<id>`) and after every world replacement such as a save load (`world-reset-<reason>`,
written on the first frame after the load so the load is inside the window). Each automatic dump
resets the window, so every scene gets its own file. A world reset that ends several scenes produces
one dump. In trace mode the copy at dump time runs on the game thread and can take tens of
milliseconds; that hitch is the profiler's, and it lands outside the dumped window.

Without the script, write the same JSON yourself (write a temporary file, then rename it to
`command.json`):

    { "command": "start", "mode": "trace", "label": "orbit", "autoDump": false, "ringEvents": 262144 }

A stale `command.json` from an earlier session is deleted at startup. A file that is not valid JSON
after three polls is renamed to `command.rejected.json`.

## Output

Everything lands in `<Documents>\My Games\Starfield\SFSE\OSF\profiles\`:

| File | Content |
| --- | --- |
| `<yyyyMMdd-HHmmss>-<label>.stats.json` | Aggregates (schema 1): capture window, frame distribution, every scope, counters, threads, allocation hot spots |
| `<yyyyMMdd-HHmmss>-<label>.trace.json.gz` | Trace mode only: Chrome trace-event JSON, gzip-compressed |
| `status.json` | Current mode, label, last command and last dump; rewritten after every command and dump |

Each dump also logs a `[Profile]` summary in the SFSE log: window and frame count, frame-time and
OSF-per-frame percentiles, the top 10 scopes by total and by maximum, the top allocation sites, and
the written paths.

### Reading stats

    pwsh -NoProfile -File tools/profiling/Show-ProfileStats.ps1 -Path "<profiles folder or .stats.json>"
    pwsh -NoProfile -File tools/profiling/Show-ProfileStats.ps1 -Path after.stats.json -Compare before.stats.json -Category Anim

`-SortBy total|max|mean|count|allocs|perFrame`, `-Top n`, `-AsJson`. With `-Compare`, each scope gets
the change in total and mean time against the baseline, which is the before/after check for an
optimization.

### Reading a trace

Open `https://ui.perfetto.dev`, choose **Open trace file**, and pick the `.trace.json.gz` (it reads
gzip directly; `chrome://tracing` also works). Tracks:

- **Frames**: one slice per game frame with `osfUs` (OSF time that began in the frame, all threads),
  `calls`, `spill` (a root scope ran past the frame end) and `gap`.
- **Game Main (...)**: the frame-maintenance thread: `Frame.*`, `MainQueue.Task` and everything the
  frame drain runs (`Scene.*`, `Prep.*`, `API.*`, Browser addon work posted through the engine).
- Job threads (named by the engine, or `thread-<id>`): `Anim.GraphUpdate`, `Anim.Compose` and their
  children, with `Lock.*` waits nested inside.
- **OSF Audio Worker** threads: `Audio.Job*`.
- Counters and gauges appear as counter tracks.

## What the numbers mean

- A **frame** is the interval between two passes of OSF's frame hook (after Main's update). **OSF
  ms/frame** sums every top-level OSF scope, on any thread, that began in that interval. Job-thread
  work runs in parallel with the game, so this is OSF's total CPU cost per frame, not its share of
  the critical path. `osfShareOfFrame` is that sum over the frame time.
- Frames longer than 250 ms (loading screens, pauses, focus loss) are **gaps**: counted, but excluded
  from the percentiles.
- `callsPerFrame` is count divided by frames in the window. `Anim.GraphUpdate` runs for every
  animated actor update in the loaded area (about 7 per actor per frame) while any OSF graph exists,
  and `Anim.Compose` for every skeleton compose. High counts there are the costs that
  `docs/SIMPLIFICATION_REVIEW_2026-10-22.md` tier 1 items 5 and 6 describe; the
  `Anim.GraphUpdate.Hit`/`.Miss` counters split them.
- `Lock.*` entries are time spent **waiting** to acquire. `*.Hold` scopes (`Anim.PlayPrepared.Hold`,
  `Anim.StopAll.Hold`, ...) are time spent holding GraphManager's exclusive state lock, during which
  every actor's animation update waits.
- Allocations count global `operator new` calls (and requested bytes) on the calling thread inside
  the scope, children included. The profiler's own first use on a thread allocates its tables and,
  in trace mode, its ring: that shows up once per thread.
- A trace ring that wraps keeps the newest events. `Show-ProfileStats` warns when a ring wrapped
  inside the window; raise `ringEvents` or dump sooner.

### Measuring the profiler's own overhead

Play the same scenario three times with `start -Mode stats`, `start -Mode trace` and `stop` (off),
dump each, and compare frame times with `Show-ProfileStats -Compare`. The off capture still records
frame times. For a comparison against the shipping DLL, use the WPR/WPA workflow below on both DLLs.

## Instrumented scopes

Names are `Category.Sub.Leaf`; the category is the text before the first dot.

| Category | Covers |
| --- | --- |
| `Frame` | The frame hook and each tick: drain, camera, fade, subtitle, graph maintenance |
| `MainQueue` | Every task the frame drain runs (`MainQueue.Task`), batch size gauge |
| `Anim` | Animation-update hook (lookup, `Sample`, mark/end queueing, flush), compose hook (`StampPose`, recovery binds, root pin), `Graph::Sample` (advance, stage switch, bind), `StampPose` (ozz sampling, unpack, live-base capture, bone writes), maintenance (anim-driven flags, stall scan, stranded sweep), exclusive-lock holds |
| `Lock` | Waits on GraphManager's state lock (shared/unique), graph locks and sync-group locks |
| `Scene` | Timed marks, fire/cue/action/sound/camera dispatch, native and Papyrus event relay, start, navigation, transitions, cover polling |
| `Prep` | Preparation start/poll/plan/commit/finish, coordinator pump, each provider and phase |
| `Load`, `Import` | Clip load (cache hit/miss counters, bytes), resource and rig reads, GLTF gunzip/sanitize/parse/skeleton/animation, AF parse/build, BA2 reads, duration probes |
| `Registry`, `Boot` | Content, sound, gear and requirement registry loads with their sub-passes, discovery, provider publish, pack reload, the kPostDataLoad steps |
| `API` | Engine services reads, JSON send size, catalog build, clip-provider rebuild, posted frame callbacks |
| `Launch`, `Match`, `Anchor`, `ActorState`, `Equip` | Scene launch, matchmaking, anchor resolution, actor-state plans and inventory reads, equip calls |
| `Save`, `Persist` | Save begin/end, equipment save window, sidecar save/load, form-delete sink, form-ID resolution |
| `Papyrus` | Every native in `OSF.psc`/`OSFAdvanced.psc` (`Papyrus.<NativeName>`) |
| `Audio`, `Input` | Sound play, worker job prepare/commit, queue depth; engaged input processing and verb dispatch |

The Browser addon DLL is not instrumented; its work that runs through the engine's frame queue is
measured as `MainQueue.Task` and `API.*`.

## Adding instrumentation

Include `Profiling/Profile.h` (the `shared` folder is on every include path):

| Macro | Production expansion | Use |
| --- | --- | --- |
| `OSF_PROFILE_SCOPE("A.B")` | nothing | Time the rest of the enclosing block |
| `OSF_PROFILE_CALL("A.B", expr)` | `(expr)` | Time one expression and yield its value; use instead of wrapping a call in a lambda |
| `OSF_PROFILE_LOCK(Type, var, "Lock.X", mutex...)` | `Type var{ mutex... }` | Declare a lock and record the wait |
| `OSF_PROFILE_COUNTER("A.B", delta)` | nothing | Running total |
| `OSF_PROFILE_GAUGE("A.B", value)` | nothing | Sampled value (min/max/last) |
| `OSF_PROFILE_TASK("A.B", callable)` | `(callable)` | Name a queued task or job; the scope opens when it runs |
| `OSF_PROFILE_THREAD_NAME("name")` | nothing | Name the calling thread in captures |

Rules:

- Names are string literals. Use a new `Category` only for a new subsystem.
- Never put side effects in macro arguments: production does not evaluate them. `OSF_PROFILE_CALL`
  is the exception; it evaluates its expression exactly once in both builds.
- Instrumentation must not change behavior: no logging, extra locking or extra work added around a
  scope. The profiler itself only allocates and takes its registry lock the first time a thread or
  site is seen.
- Limits: 512 instrumented sites and 256 threads per process; extra ones are counted as dropped and
  reported in the stats.
- `src/Profiling/ProfilerHost.h` (`OSF_PROFILE_HOST_*`) is only for the frame hook, plugin load and
  the automatic dump points.

## CPU sampling with WPR/WPA

Windows Performance Recorder and Analyzer are part of the Windows Performance Toolkit. Start
Starfield normally, load the state to measure, then run an elevated PowerShell from this repository:

    .\tools\profiling\Capture-Cpu.ps1 -Label camera-orbit

Reproduce for 10-20 seconds and press Enter in the PowerShell window. The script saves a compressed
ETL under build/profiles, configures the local normal and profiling-override PDB directories ahead of
Microsoft's symbol server, and opens WPA. In WPA:

1. Add **Computation > CPU Usage (Sampled)**.
2. Filter Process to Starfield.exe.
3. Load symbols, group by Stack, and find OSF Animation.dll frames.
4. Use inclusive samples to find expensive call trees and exclusive samples to find leaf work.

Recording has system-wide sampling overhead only while WPR is active. If capture fails, the script
cancels its named WPR session instead of leaving a recorder running.

References: [WPR command line](https://learn.microsoft.com/windows-hardware/test/wpt/wpr-command-line-options),
[WPA symbols](https://learn.microsoft.com/windows-hardware/test/wpt/loading-symbols),
[Perfetto UI](https://ui.perfetto.dev), [trace event format](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU).
