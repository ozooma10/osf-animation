**Clip availability optimization — October 22, 2026**

The registry now probes clip candidates lazily. An installed primary resource no longer requires constructing the unused NAF/OSF fallbacks or full filesystem paths for resource candidates. Evidence is retained under [build/profiles/clip-availability-20261022](../build/profiles/clip-availability-20261022).

**Outcome:** the reload improvement is verified and functional checks passed. **Steady-playback performance clearance remains open:** the final paired AF8 measurements were higher than both neighboring controls. This report does not certify the change as performance-regression-free.

**Why this target.** The preceding Browser parsing session's visible reload trace attributed 69.419 ms of self time across two reloads to `Registry.LoadAll.SweepClips`, with 215,238 allocations per reload. It was the largest remaining instrumented leaf scope on the game thread's registry reload path; the larger full catalog callback ran on the Browser metadata worker and was the previous optimization's target. A fresh control confirmed 34.24–34.28 ms in two closed-browser reloads and 36.60–37.69 ms in two visible reloads.

**Implementation.** `Util::ProbeClipSpec` and the existing materialized `ResolveClipSpec` share one candidate enumerator. Mounted-resource callbacks receive only resource names and stop on success. Full filesystem paths are constructed only for materialized playback candidates; absolute paths outside Data still use filesystem existence checks. Absolute paths within Data retain the existing containment and normalization rules. Candidate precedence, authored spelling, explicit NAF handling, missing-clip diagnostics and load-local positive/negative caching are unchanged. There is no persistent availability cache or new game-thread/worker ownership change.

The first commit added regression coverage against the original implementation. It verifies successful primary/fallback lookup, misses, external paths, per-load deduplication, installed siblings in partially missing scenes, Imports ownership/counts, registered clip reuse, and removal/reinstallation on subsequent loads. The implementation commit also adds fixed expected candidate fixtures for relative, explicit NAF, Data-contained absolute, external absolute, UNC, drive-relative and empty specs, checking both full playback paths and short-circuit probes.

**Measurements and live regression checks.**

The first focused comparison used the same installed content: 953 authored scenes, 38 files, 179 Browser catalog entries and 1,077 library entries. Two successful visible reloads preserved the complete sorted ID sets and one overlapping request was rejected. Catalog and library fingerprints remained `53c9f9b8e0322db8` and `5230e3c813f44e93`. The [optimized screenshot](../build/profiles/clip-availability-20261022/runs/20261022-191120-762-ComprehensiveProfiling/browser-catalog-refresh.png) was inspected and shows populated results and the successful reload confirmation.

| First focused comparison | Control | Optimized |
|---|---:|---:|
| Closed-browser availability sweep, two runs | 34.24–34.28 ms | 22.02–22.21 ms |
| Visible availability sweep, two runs | 36.60–37.69 ms | 22.21–22.29 ms |
| Sweep allocations per reload | 215,238 | 68,867 |
| Sweep allocated bytes per reload | 26,329,627 | 6,201,771 |
| Closed-browser complete registry reload | 165.69–172.48 ms | 148.97–153.64 ms |
| Visible request → refreshed report | 272.65–287.85 ms | 249.10–249.13 ms |
| Visible reload maximum frame-hook interval | 185.57 ms | 157.36 ms |

The sweep eliminated **146,371 allocations (68.0%)** and **20,127,856 allocated bytes (76.4%)** per reload. These counts come from engine DLL allocation hooks and are cumulative allocation traffic, not peak memory or engine-wide allocations. Inclusive scope totals must not be added to their children; frame-hook intervals are not rendered FPS.

The first comparison had different sampled focus states: the control was background and the optimized run foreground. Its camera-service mean rose from 1.26 to 8.62 µs in the camera case, and from 1.36 to 9.36 µs in the equipment case. `DriveSceneOrbit` polls desktop keys only while the game is foreground, so this is a material comparison difference. The raw captures and higher results are retained; `compare-steady.py` flags both focus and hook-cadence mismatches. A follow-up control checks the foreground cases. The allocation reduction does not depend on interpreting these timing differences.

The follow-up control sampled the game foreground, matching the optimized focused run. It measured 34.25–36.72 ms for closed-browser sweeps, versus 22.02–22.21 ms optimized. Visible sweeps averaged 34.66 → 22.25 ms (**35.8% lower**), and visible completion averaged 269.40 → 249.12 ms (**7.5% lower**). Closed reloads execute through the Papyrus call on a job thread; the visible Browser reload executes on Game Main. Catalog construction, serialization and the Browser callback remain on the metadata worker, as recorded in `trace-threads.json`.

Matching focus also reproduced the foreground camera cost in the control: 8.89 → 8.62 µs per camera tick and 0.03161 → 0.03090 ms total OSF work per frame-hook record. Equipment was noisier in the control (17.27 → 9.36 µs camera tick); the two eight-actor focused repetitions had lower means after the change, although the second p99 remained higher (0.1870 → 0.2223 ms). This is timing evidence from short windows, not proof of a steady-playback speedup from a registry-only change. Focus is sampled at window boundaries, not continuously.

The complete original scenario matrix passed: idle, one/two/eight AF actors with repetitions, AF8 off/stats/trace overhead windows, GLB and warm GLB, transitions, effects, equipment, additive playback, two managed actors alongside sixteen unmanaged actors, repeated reloads, save/load and final idle. It retained 63 stats windows and 12 traces. All six optimized reloads used the same 68,867 allocations and 6,201,771 bytes; closed sweeps in the full run were 22.01 and 22.96 ms.

The broad comparison with the preceding session had three higher steady means: second AF2 repetition 0.02482 → 0.04687 ms, second AF8 repetition 0.11573 → 0.14894 ms, and first GLB 0.02911 → 0.04343 ms. These captures remain in the report rather than being discarded. That comparison also had different sampled focus states. A fresh paired timing run repeats AF2 twice, cold/warm GLB and the AF8 off/stats/trace sequence with frozen binaries and identical copied fixtures.

All paired timing windows sampled foreground and had comparable hook cadence (about 120 records/second). A control → optimized → control sequence retained these mean OSF milliseconds per frame-hook record:

| Window | Control before | Optimized | Control after |
|---|---:|---:|---:|
| AF2 repetition 1 | 0.02277 | 0.02728 | 0.02367 |
| AF2 repetition 2 | 0.02290 | 0.02394 | 0.02387 |
| First GLB | 0.02410 | 0.02731 | 0.02523 |
| Warm GLB | 0.02443 | 0.02471 | 0.02392 |
| AF8 stats 1 | 0.09722 | 0.11285 | 0.09339 |
| AF8 stats 2 | 0.09443 | 0.12396 | 0.09224 |
| AF8 trace 1 | 0.10348 | 0.12020 | 0.09875 |
| AF8 trace 2 | 0.10134 | 0.10405 | 0.09416 |

The large full-run AF2/GLB increases did not recur at that magnitude. However, the paired AF8 stats means were **16.1–34.4% higher** than the neighboring controls (15.6–31.7 µs more per record), and some smaller AF2/GLB differences also remained. The earlier optimized full run had substantially lower AF8 stats means, 0.09596 and 0.09625 ms, so the recordings show variation even with the same optimized DLL. The availability sweep does not run in these steady windows, but that alone does not establish the cause of the difference. The repeatable reload gain and passing functional assertions do not close this playback timing concern. Raw windows, p99 values, per-scope timings, focus samples and process telemetry are retained for further investigation; no slow window was removed.

**Build and test evidence.** All 15 native suites passed before the implementation. After the change, all 16 currently configured native suites passed; an independent layer-composition suite was added in the shared workspace during this task. That unrelated work is excluded from these commits. Browser boundary checks passed, as did all 166 UI tests, TypeScript checking and the production UI build after `npm ci`.

Production was built/deployed in releasedbg with `--profiling=n --animation_smoke=n`. The profiler override was built through the dedicated script and production configuration restored. The tested DLL/PDB pairs were frozen under `tested-binaries`, and 248 source/configuration files were retained with hashes. The optimized live controller uses those frozen DLLs in its disposable payload, preserving the original fixture cases, order and durations. The paired timing checks additionally freeze the copied `dist` payload. The installed Browser DLL is unchanged from the previous optimization. New layer-composition code was not referenced by production sources at the tested checkpoint.

Independent work continued editing the shared checkout and subsequently rebuilt the production output. `source-drift.json` records differences from the tested snapshot. Validation here applies to the retained source and DLL identities below, not to all later workspace edits or newer shared build outputs. Those changes are preserved and excluded from this task's commits.

| Tested DLL | SHA-256 |
|---|---|
| Production engine | `482715C3A518E8FCBD890980AD86B7E90FA20515CADD7483FE08D6831D00CCB6` |
| Browser | `1B136BEDE0CBF355B6C5911554FF603186415B9A72FA248E77E174A909370F3D` |
| Profiling engine | `A57349031CFECEA8A4997C3E844CAA7F6F66CE49CD32F9D720BA8D4A67C521EE` |

Seven live runs passed their functional assertions, retaining **195 stats windows and 30 traces**. Across those windows there were no wrapped rings, torn events, dropped descriptors or dropped threads. Startup/load frame gaps remain in the raw data; closed and visible reload windows had no gaps above the profiler's 250 ms threshold.

| Run ID (all under the evidence root's `runs` directory) | Purpose | Stats / traces |
|---|---|---:|
| `20261022-190252-618-ComprehensiveProfiling` | Initial baseline, focused + reload + Browser | 20 / 4 |
| `20261022-191120-762-ComprehensiveProfiling` | Optimized focused + reload + Browser | 20 / 4 |
| `20261022-191524-189-ComprehensiveProfiling` | Optimized full matrix | 63 / 12 |
| `20261022-192835-168-ComprehensiveProfiling` | Foreground baseline confirmation | 20 / 4 |
| `20261022-193245-876-ComprehensiveProfiling` | Timing control before | 24 / 2 |
| `20261022-193748-177-ComprehensiveProfiling` | Timing optimized | 24 / 2 |
| `20261022-194338-576-ComprehensiveProfiling` | Timing control after | 24 / 2 |

Final verification at **19:49:27 UTC** confirmed the deployed engine, Browser and profiler DLLs and PDBs match the frozen tested copies. The separate profiling mod is disabled in Default, production is releasedbg with profiling/smoke disabled, all seven temporary profiles/payloads were removed, the game and MO2 are closed, and the shared runner lock is available. The production engine has no profiling marker; only the separate override has it. The normal Browser view contains no private probe.

Newer production engine and Browser outputs exist in the shared build directory from independent work; they differ from the installed/tested files and were not substituted into these runs or deployed by this task. `final-verification.json` records both sets of hashes. The report's evidence is bounded to the frozen binaries above.

Live captures exercise the profiling engine and production Browser DLL. The off-mode windows disable collection within the profiling build; they are not a separate live acceptance run of the production engine DLL. Production build/deployment checks and the engine-free native suites are separate evidence.

Live Browser checks use the installed view (`index-wVElcjZH.js`). UI source verification separately builds `index-BuXpY8Gr.js`; native xmake does not deploy generated Vite assets. The private acceptance probe is added only to disposable view copies. Physical input, audible output, populated-city load and large persistence payloads are outside these scenarios; audio uses the existing silent fixture and save/load uses quiescent empty client records.

**Commit slices.**

- `a00c542` — regression coverage, verified against the original implementation.
- `80a9d9f` — lazy clip-availability probing and playback-path compatibility tests.
- This report records the final measurement and validation slice.

Earlier uncommitted optimizations and unrelated design/layer work remain outside these commits. No push or package was made.
