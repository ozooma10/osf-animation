**Catalog refresh optimization and verification — October 22, 2026**

The measured background catalog hitch is removed from the game thread. Its frame callback fell from **185–189 ms to 0.152–0.165 ms** on the final deployed pair. Full catalog construction, serialization and parsing still take approximately **152–167 ms**, now on a Browser-owned metadata worker. This change improves scheduling and avoids rebuilding static metadata for duration-only updates; it does not claim that the full catalog's CPU/allocation cost has disappeared.

The original findings remain in [the profiling session report](PROFILING_SESSION_2026-10-22.md). All implementation, tests, builds, captures and comparisons for this follow-up are retained under [build/profiles/catalog-refresh-20261022](../build/profiles/catalog-refresh-20261022).

**Implementation.** [CatalogRefreshQueue](../extensions/browser/src/API/CatalogRefreshQueue.h) owns one coalescing worker. A newer request supersedes an in-flight or unpublished result. Failed reads preserve the last good catalog and remain retryable. Only a verified frame command adopts completed metadata; nested getters cannot replace a snapshot while callers hold its references. Retired snapshots are destroyed on the worker. World-reset handling resumes a completed publication if its original frame callback was canceled.

[EngineClient](../extensions/browser/src/API/EngineClient.cpp) performs the full JSON round trip and Browser decoration on that worker. Duration-scan completion now invalidates only the duration overlay. Initial and external-reload completion refresh visible catalog/library/imports/wheel data. An Imports reload response waits for the new report; its existing document requests fetch the new catalog/library without duplicate native pushes.

Services **3.16** explicitly permits metadata-only `ReadCatalog` calls from consumer workers. The handler pins an immutable registry snapshot, and its only shared mutable input is the mutex-protected decoded-duration store. No game objects are read. The table layout and schema-3 JSON remain unchanged; the updated Browser refuses older hosts before starting worker reads. Existing supported older consumers retain their prefix compatibility. See [OSFEngineAPI.h](../src/API/OSFEngineAPI.h) and [Browser API/lifetime documentation](BROWSER_ADDON.md).

**Primary comparison.** The original and first optimized matrix both used 952 authored scenes from 38 files. Final confirmation used the same set plus the camera fixture, totaling 953 scenes. Each column contains two reload captures.

| Measurement | Original | Optimized, matched matrix | Final deployed pair |
|---|---:|---:|---:|
| Largest catalog-related frame callback | 185.304–188.795 ms | 0.155–0.163 ms | 0.152–0.165 ms |
| Longest non-gap frame-hook interval in capture | 187.491–190.969 ms | 12.943–13.401 ms | 9.138–12.509 ms |
| Full `API.ReadCatalog` | 145.907–153.116 ms, Game Main | 152.918–166.212 ms, worker | 151.896–167.326 ms, worker |

The callback reduction exceeds **99.9%**. Thread attribution in the traces, not merely the aggregate scope total, establishes the change. `osfMsPerFrame.max` still includes the long worker operation and is unsuitable as a rendered-frame critical-path metric. Hook intervals are also not PresentMon/GPU presentation measurements. The approximately 6.13 MB transfer and roughly 736,000 tracked allocations per full read remain opportunities for a later serialization/provider optimization.

Measurement clarification from the [registry/provider follow-up](REGISTRY_PROVIDER_OPTIMIZATION_2026-10-22.md): the profiler classifies intervals over 250 ms as gaps and excludes them from `frameMs.max`. The table demonstrates removal of the catalog frame callback; it does not establish that whole reloads became free of long stalls. The follow-up reports gap durations alongside ordinary frame intervals and reduces the remaining registry/provider work.

**Regression checks.** The full original workload matrix passed: idle and profiler modes; repeated one/two/eight-actor AF playback; cold/warm GLB; transitions; cues/audio; equipment/control; additive poses; unmanaged-actor density; two content reloads; quiescent save/load; final idle. Camera and positive/negative matchmaking batches passed in supplemental sessions. The native suite passed **15/15**, including new tests for blocked reads, coalescing, stale completion rejection, failure/retry, preserved scan intent, publication cancellation/resumption, old-host rejection and duration updates without another catalog read. The Browser boundary check passed.

Selected mean instrumented OSF time per hook from the original versus optimized main matrix:

| Workload | Original | Optimized |
|---|---:|---:|
| 1 AF actor, two repeats | 0.0130–0.0138 ms | 0.0132–0.0134 ms |
| 2 AF actors, two repeats | 0.0239–0.0242 ms | 0.0238–0.0243 ms |
| Node transitions | 0.0325 ms | 0.0311 ms |
| Transitions with cues/audio | 0.0349 ms | 0.0344 ms |
| Additive poses | 0.0551 ms | 0.0527 ms |
| Warm GLB | 0.0260 ms | 0.0260 ms |

The initial equipment and eight-actor windows were slightly higher, so they were not dismissed as noise. The backed-up original DLL pair was then run in a fresh disposable profile, followed by the final optimized pair with the same camera/equipment/eight-actor sequence:

| Matched fresh controls | Original binaries | Final optimized binaries |
|---|---:|---:|
| Equipment mean | 0.03222 ms | 0.03160 ms |
| Equipment p99 | 0.0489 ms | 0.0463 ms |
| 8 actors, mean across two repeats | 0.10761–0.11384 ms | 0.10412–0.10832 ms |
| 8 actors, p99 across two repeats | 0.1645–0.2002 ms | 0.1436–0.1732 ms |

The earlier equipment difference was in camera maintenance: the original session's camera tick averaged 1.30 µs, while both original and optimized fresh controls were near 6–7 µs. Pose writing remained near 4 µs per stamp. No sustained slowdown attributable to this change was identified. These bounded repetitions do not establish a precise universal speedup for unchanged playback paths. Raw main-matrix deltas, including higher windows, are retained in `steady-comparison.csv`; normalized elapsed time per second remains available where hook cadence differs.

**Visible browser acceptance.** A private script was added only to the disposable copy of the existing view. It used the public bridge, without Windows input, to observe metadata, issue two successful Imports reloads, reject one overlapping reload, and compare catalog/library identities after both reloads. It passed with **179 catalog entries, 1,077 library entries, 38 files and 953 authored scenes**. The [captured browser](../build/profiles/catalog-refresh-20261022/runs/20261022-161245-380-ComprehensiveProfiling/browser-catalog-refresh.png) was inspected and shows populated results and reload confirmation. The script and injected view were not deployed to the normal Browser mod.

Explicit Imports reload still performs the existing `Registry.ReloadAll` on the game thread: this acceptance capture measured approximately **299 ms** for that work, and **510–525 ms** from request through the refreshed report. Moving the catalog read does not remove registry/provider rebuild work or the visible view's presentation serialization. Those remain separate optimization targets.

**Retained failures.** The first test compile encountered the known sandbox PDB-manager mismatch (`C1902`); the normal build environment then passed both builds and all suites. The first visible-browser fixture failed because OSF UI's developer reload explicitly canceled the open. Its mutation requests were correctly rejected while hidden. That failed run remains failed. The corrected fixture waited for visibility and reopened only after observing the explicit cancellation; the recapture passed. No game crash was observed. The original session's earlier controller failures remain documented in its separate report.

**Evidence.** Five launches produced **121 distinct stats windows and 18 traces**: 104 windows on optimized binaries and 17 on the original-binary control. None reported wrapped rings, torn events, dropped descriptors or dropped threads.

| Run | Scope | Outcome / captures |
|---|---|---|
| `20261022-155052-331-ComprehensiveProfiling` | Matched full matrix | Passed; 63 stats, 12 traces |
| `20261022-160537-874-ComprehensiveProfiling` | Camera, queries, equipment/eight-actor repeats, first browser fixture | Fixture failed as described; 17 stats, 1 trace retained |
| `20261022-161245-380-ComprehensiveProfiling` | Corrected browser acceptance and queries | Passed; 5 stats, 1 trace |
| `20261022-161451-386-ComprehensiveProfiling` | Backed-up original binaries, fresh controls | Passed; 17 stats, 1 trace |
| `20261022-161850-283-ComprehensiveProfiling` | Final optimized pair, matched controls and two reloads | Passed; 19 stats, 3 traces |

The first matrix used Browser hash `44FD8CD841A3C6DE4F15F90320A3B055CC1452B462C4DCAFCC83CEDAEB23EBCD`. A subsequent visible-only fix suppressed duplicate catalog/library pushes after an Imports reload response. The final Browser was used by the remaining optimized runs, including the successful visible acceptance and final closed-browser reload captures. The engine/profiling binaries stayed identical throughout the optimized runs. Each run retains exact binary identities and workload data; `source/` and `source-manifest.json` preserve the final implementation. `analyze.py` and `compare.py` reproduce the summaries.

**Deployment and limits.** Normal production DLLs were built/deployed with `releasedbg`, `profiling=n`, `animation_smoke=n`. Final built/deployed hashes match:

- Engine: `F07965DB88266361904050B3AF08BAC5549BDA0ACFE2B5D7C029DBCEA5A571D6`
- Browser: `62100DF4FC5695C2E0D742AD3ECBC293B20C2AC71A6139B7E051FCD22DBA17F0`
- Separate profiling override: `B924489ECB455237F6BA7F0FF8388280DCD9637A7421F55A5D49FD822C710DA3`

[Final verification](../build/profiles/catalog-refresh-20261022/final-verification.json) confirms that the normal engine has no profiling marker, the override is disabled in Default, Default is selected, all five temporary-profile journals report removal, and no temporary mod/profile or Starfield/MO2 process remains. Original binaries are backed up under the evidence folder. No commit, push or package was made.

The live save/load was quiescent and used empty client records; cancellation during a pending catalog refresh is covered by the native test. The silent audio fixture, private test cell and limited clip set retain the original report's scope limits. Physical controller/camera interaction, large persistence payloads, populated-city load and rendered FPS were not qualified by these measurements.
