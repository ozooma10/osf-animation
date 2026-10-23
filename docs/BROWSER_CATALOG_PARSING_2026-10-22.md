**Browser catalog JSON parsing — October 22, 2026**

Browser now decodes the full catalog directly into its owned typed snapshot, eliminating the intermediate generic JSON tree and subsequent conversion pass. Evidence, previous binaries, tested sources, controllers and comparison scripts are retained under [build/profiles/browser-parse-20261022](../build/profiles/browser-parse-20261022).

**Implementation.** [BrowserCatalogJson](../extensions/browser/src/API/BrowserCatalogJson.cpp) consumes the existing nlohmann SAX interface. The library still validates JSON syntax, UTF-8, escapes and numeric tokens. A mutable field visitor uses the same `OSFCatalog.h` declarations as the ordinary converters and engine serializer, so there is no duplicate schema list. Strings and nested arrays belong to the resulting snapshot; no pointers into the callback text survive parsing.

Each object tracks its required fields. Unknown fields are skipped while their JSON syntax remains validated. Numeric conversions use the existing JSON conversion rules. Duplicate known fields or mismatched types fall back to the previous DOM decoder, preserving last-value behavior even when an earlier duplicate contains invalid or incomplete metadata. Failed decoding never publishes partial data. Tests exercise rejection, last-good retention and recovery through the production Browser refresh worker.

The caller-thread, coalescing, stale-result rejection, frame publication, retired-snapshot destruction, duration-overlay and world-reset contracts are unchanged. Decorating remains on the metadata worker after decoding. Compact provider input parsing is unchanged. Services remain 3.17 and the full catalog remains schema 3.

**Measurements.** Fresh control and optimized focused runs used the same 953 authored scenes, 38 files, installed Browser view, fixtures, case order and capture durations. The control used the backed-up streaming-serializer pair from the preceding session. Each closed-browser row covers two reloads; the visible check performs two successful Imports reloads.

| Measurement | Previous deployed pair | Typed SAX parser |
|---|---:|---:|
| Catalog callback | 43.306–48.155 ms | 38.221–38.866 ms |
| Full engine `ReadCatalog` call | 90.575–99.589 ms | 86.153–87.965 ms |
| Visible request → refreshed report | 319.265–323.475 ms | 259.749–268.295 ms |
| Complete registry reload | 164.283–169.491 ms | 164.124–172.561 ms |
| Closed-browser reload frame interval, maximum | 172.901–177.951 ms | 172.630–181.433 ms |
| Reload frame gaps over 250 ms | 0 | 0 |

Mean visible request-to-report time fell **17.8%**. The callback fell **15.7%**, despite doing more work within that scope: it previously created only the DOM; it now also produces the complete typed snapshot. The old typed conversion and DOM destruction happened after `ReadCatalog` returned, so that call's **8.4%** reduction does not capture the whole Browser saving. Visible completion is the comparable end-to-end measurement. Payloads remain approximately 6.13 MB with the same complete schema and IDs.

An isolated decoder benchmark compares equivalent work: parse, produce the complete typed snapshot, and destroy all owned data. On the same **6,042,886-byte synthetic catalog**, seven alternating runs measured median **79.726 ms → 36.470 ms**, a **54.3%** reduction. Ranges were 78.781–81.471 ms and 35.824–37.455 ms. This is synthetic evidence, distinct from the live catalog. Reproduce it with `xmake run osf-browser-boundary-test --catalog-bench`; `parser-benchmark.log` retains the output. It is an opt-in benchmark, not a timing threshold in the test suite.

Trace attribution confirms the full read and typed decode remain on the Browser metadata worker. Explicit Imports registry reload remains on Game Main; this change does not remove its synchronous work. Across the optimized focused and full runs it took approximately 164–188 ms. The profiler's allocation hooks cover only the engine DLL, not Browser allocations. No numerical Browser allocation or peak-memory reduction is claimed. Nested scope times must not be summed, and frame-hook intervals are not rendered FPS. Intervals over 250 ms are recorded separately as gaps.

**Regression checks.** Fresh focused playback passed camera, positive/negative batches of 100 queries, equipment/control and two eight-actor repetitions:

| Focused window | Previous mean | New mean | Previous p99 | New p99 |
|---|---:|---:|---:|---:|
| Camera | 0.026982 ms | 0.027008 ms | 0.0389 ms | 0.0435 ms |
| Equipment | 0.029918 ms | 0.027508 ms | 0.0465 ms | 0.0400 ms |
| Eight actors, repeat 1 | 0.116971 ms | 0.113640 ms | 0.1935 ms | 0.1701 ms |
| Eight actors, repeat 2 | 0.119159 ms | 0.112443 ms | 0.2136 ms | 0.1644 ms |

Camera mean was nearly unchanged (+0.1%) and its p99 was higher; the other focused windows were lower. These bounded measurements are retained as regression evidence, not claimed as playback benefits from parsing.

The full matrix also passed: idle, one/two/eight actors with two repetitions, profiler off/stats/trace, GLB import and warm replay, transitions, effects, equipment/control, additive playback, two managed actors with 16 unmanaged actors, two registry reloads, quiescent save/load and final idle. Its two catalog callbacks measured 38.944 and 39.887 ms, supporting the focused parsing result.

Compared with the preceding serialization session's full run, several playback means were higher. These results are retained, including the less favorable measurements:

| Full-run window | Previous mean → new mean | Previous p99 → new p99 |
|---|---:|---:|
| Two actors, repeat 1 | 0.023957 → 0.026059 ms | 0.0365 → 0.0408 ms |
| Eight actors, repeat 1 | 0.108287 → 0.113325 ms | 0.1572 → 0.1711 ms |
| Eight actors, trace 1 | 0.109601 → 0.129094 ms | 0.1633 → 0.2422 ms |
| Eight actors, trace 2 | 0.109013 → 0.130334 ms | 0.1656 → 0.2340 ms |
| GLB | 0.025757 → 0.029105 ms | 0.0369 → 0.0450 ms |
| Warm GLB | 0.026005 → 0.027225 ms | 0.0380 → 0.0427 ms |
| Transitions | 0.029878 → 0.031889 ms | 0.0796 → 0.0829 ms |
| Effects | 0.033254 → 0.035202 ms | 0.1019 → 0.1002 ms |
| Equipment | 0.027276 → 0.027982 ms | 0.0416 → 0.0434 ms |
| Additive | 0.053762 → 0.056067 ms | 0.0802 → 0.0812 ms |
| Crowd density | 0.053652 → 0.056358 ms | 0.0790 → 0.0756 ms |
| Final idle | 0.001547 → 0.001473 ms | 0.0033 → 0.0032 ms |

The two eight-actor trace means were 17.8–19.6% higher. Scope inspection located the difference in animation sampling and pose work, with equivalent call counts and no catalog parsing active in those windows. Because the fresh focused eight-actor comparison went in the other direction, a further matched control/optimized pair repeated two-actor playback, initial/warm GLB and the original six off/stats/trace windows. The complete comparisons, including both stats overhead windows and both actor repetitions, are in `steady-comparison.csv`.

The fresh timing pair passed and produced these comparisons:

| Matched follow-up window | Control mean → optimized mean | Control p99 → optimized p99 |
|---|---:|---:|
| Two actors, repeat 1 | 0.025462 → 0.024810 ms | 0.0393 → 0.0382 ms |
| Two actors, repeat 2 | 0.024943 → 0.024886 ms | 0.0385 → 0.0372 ms |
| Initial GLB | 0.028578 → 0.027297 ms | 0.0434 → 0.0417 ms |
| Warm GLB | 0.027710 → 0.028812 ms | 0.0407 → 0.0482 ms |
| Eight actors, stats 1 | 0.125368 → 0.118741 ms | 0.2462 → 0.2092 ms |
| Eight actors, stats 2 | 0.116380 → 0.118099 ms | 0.1813 → 0.2035 ms |
| Eight actors, trace 1 | 0.125724 → 0.123872 ms | 0.1841 → 0.1710 ms |

The second control trace had **29,209 frame-hook records in 16.09 seconds** (1,815/s), versus approximately 120/s in the other matched windows. Its per-frame mean and p99 are not a valid comparator; the comparison script explicitly flags this cadence mismatch. The capture is retained. On that window, `Anim.GraphUpdate` mean per call was 4.363 → 4.108 µs, `Anim.Compose` was 11.176 → 10.915 µs, and the sum of the three independent top-level scope totals was 15.435 → 14.669 ms per captured second. These measures do not suggest an animation-work increase, but they do not explain the abnormal hook cadence. Both first trace windows also contained a long scope outlier; their full distributions and scope totals remain in the evidence.

The larger eight-actor increase did not repeat in the usable fresh comparisons. Warm GLB remained **4.0% higher** in the extra pair (+1.102 µs mean, +7.5 µs p99), and the second eight-actor stats mean was 1.5% higher. No functional regression was observed. The remaining small timing differences and the control cadence anomaly are not causally resolved; these results do not establish universal timing equivalence or a playback speedup.

Visible Browser acceptance passed with **179 catalog entries, 1,077 library entries, 38 files and 953 authored scenes**. Two reloads preserved complete sorted ID sets, and an overlapping reload was rejected. Control and optimized fingerprints match exactly: catalog `53c9f9b8e0322db8`, library `5230e3c813f44e93`. The [optimized capture](../build/profiles/browser-parse-20261022/runs/20261022-182808-404-ComprehensiveProfiling/browser-catalog-refresh.png) was inspected and shows populated results and successful reload confirmation.

All **15 native suites** passed. New differential tests compare against the previous decoder across every record's required fields and types, engine/alphabetical/reversed key order, unknown nested fields, duplicate overwrites, integer limits, numeric/bool conversion, float extremes, Unicode/control escapes, malformed UTF-8/surrogates/syntax, trailing/truncated input, parent-vector growth, borrowed-text lifetime and concurrent independent parses. The production-worker test verifies failure retention and subsequent recovery. Existing provider, transport, coalescing and world-reset tests remain in place. Browser boundary checks passed. The Browser's **166 UI tests**, TypeScript check and production build passed with locked dependencies.

**Evidence and deployment.**

Five launches passed, retaining **151 stats captures and 24 traces**. The optimized binaries account for 107 stats captures and 18 traces; the remaining captures are the two fresh controls. All captures report zero wrapped rings, torn events, dropped descriptions and dropped threads. These transport checks do not establish that capture cadences are comparable.

| Run | Purpose | Stats / traces |
|---|---|---:|
| `20261022-182416-457-ComprehensiveProfiling` | Previous deployed pair: focused, reloads, visible Browser | 20 / 4 |
| `20261022-182808-404-ComprehensiveProfiling` | Optimized pair: same focused and Browser checks | 20 / 4 |
| `20261022-183215-069-ComprehensiveProfiling` | Optimized pair: full original matrix | 63 / 12 |
| `20261022-184525-782-ComprehensiveProfiling` | Previous pair: matched timing confirmation | 24 / 2 |
| `20261022-185047-304-ComprehensiveProfiling` | Optimized pair: matched timing confirmation | 24 / 2 |

`Run-Session.ps1` retains the original matrix and focused switches. `Run-TimingChecks.ps1 -QueriesOnly` runs the extra timing subset, with `-BaselineBinaries` selecting the backed-up control. Each run retains its controller, fixture, binary identity, logs, captures and cleanup journal. `analyze.py`, `compare-provider.py`, `compare-steady.py` and `trace-threads.py` produce the retained summaries. `full-trace-timing-investigation.json` records the initial scope comparison. Captures predating each run are excluded from stats comparisons.

Production was built and deployed using releasedbg with `--profiling=n --animation_smoke=n`. The profiling build was deployed only to its separate override and production configuration restored. Final deployed DLL hashes match their tested build outputs:

| DLL | SHA-256 |
|---|---|
| Production engine | `AD52088A276B19A0843C89F767F6B0B6DEAD6B05CEC5D6AACCA77F45BA497AAD` |
| Browser | `1B136BEDE0CBF355B6C5911554FF603186415B9A72FA248E77E174A909370F3D` |
| Profiling engine | `F2E845F6BC92C0D8349670E0CB6489C4619BC21C0512229663E5CDCF7D804DB8` |

`final-verification.json` confirms production configuration, the profiler marker only in the override, Default selected, the override disabled, all disposable profiles/mods removed, and Starfield/MO2 stopped. The 17 retained tested-source hashes match the current files. Previous binaries remain under `before-build`, and implementation inputs under `source-before`. Existing unrelated working-tree changes were preserved.

Live Browser acceptance used the installed bundle (`index-wVElcjZH.js`). Current-source UI verification separately produced `index-BuXpY8Gr.js`; native xmake deployment does not publish Vite output. Final verification records both manifests. Private acceptance scripts were confined to disposable view copies.

Physical input, audible output, populated-city load and large persistence payloads remain outside this qualification. The audio fixture is silent, and save/load uses quiescent state with empty client records. No commit, push or package was made.
