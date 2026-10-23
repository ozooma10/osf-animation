**Catalog migration repair and focused AF8 retest — October 22, 2026**

The migrated checkout passes all 16 native suites, all 166 Browser UI tests, TypeScript checking, the UI build and the Browser boundary check. Both native DLLs build successfully through xmake. The short eight-actor comparison did not reproduce a consistent 16–34% slowdown: one stats window was higher, the other lower, and their simple average was 0.9% higher. Earlier slower captures remain retained; this is a limited follow-up, not blanket performance clearance.

**Repair slice — `acd0c84`.** The previously reported `EngineClient` uses of `Catalog::Scene` and `Snapshot.scenes` had already been migrated in the shared checkout when this task began. The remaining `UIBridgeCatalog.h` forward declaration still named `Scene` while its signatures used `Node`. The repair makes the declaration and signatures consistent. The boundary test now includes that header before `EngineClient.h`, preventing the latter from hiding a missing forward declaration. Only this header and the test include order were committed; other migration and optimization changes remain separate.

Evidence is retained under [build/profiles/catalog-migration-retest-20261022](../build/profiles/catalog-migration-retest-20261022): native/UI/build logs, initial workspace diff, staged DLL identities, controller, run artifacts, `focused-results.json` and `final-verification.json`. Tests and the build apply to the shared working tree at the recorded run, including its existing uncommitted migration work.

**Focused comparison.** To isolate the earlier clip-availability optimization from the concurrent V2 migration, the live check used the same frozen pre/post-optimization profiler DLLs and frozen fixture payload as the prior investigation. Each run used eight AF actors continuously, two 20-second stats windows around one 15-second trace window, and the existing disposable save/profile controller. The two runs took about four minutes combined.

| Window | Control mean OSF ms/record | Optimized mean OSF ms/record | Change | Control p99 | Optimized p99 |
|---|---:|---:|---:|---:|---:|
| Stats 1 | 0.12241 | 0.13438 | +9.8% | 0.2290 | 0.2838 |
| Trace | 0.12910 | 0.12491 | −3.2% | 0.2200 | 0.2065 |
| Stats 2 | 0.12694 | 0.11723 | −7.6% | 0.2404 | 0.1939 |

The simple average of the two stats means was **0.12467 → 0.12580 ms (+0.9%)**. These are OSF CPU timings per frame-hook record, not rendered frame times or FPS. Both runs sampled background focus at each window boundary and maintained comparable hook cadence, 119.81–119.96 records/second. Focus was not monitored continuously. All six measured windows had zero gaps over 250 ms. All retained captures had zero wrapped rings, torn events, dropped descriptors and dropped threads. The first optimized window's higher mean and p99 are retained, not discarded.

Both runs passed their functional assertions, retaining 14 stats captures and two traces:

- `20261022-202714-088-FocusedAF8` — control, seven stats captures and one trace.
- `20261022-202911-530-FocusedAF8` — optimized, seven stats captures and one trace.

| Live binary | SHA-256 |
|---|---|
| Control profiling engine | `F2E845F6BC92C0D8349670E0CB6489C4619BC21C0512229663E5CDCF7D804DB8` |
| Optimized profiling engine | `A57349031CFECEA8A4997C3E844CAA7F6F66CE49CD32F9D720BA8D4A67C521EE` |
| Browser, both runs | `1B136BEDE0CBF355B6C5911554FF603186415B9A72FA248E77E174A909370F3D` |

**Current native build.** Xmake installed the newly built migration DLLs into private validation staging, keeping the installed runtime unchanged. These are distinct from the frozen live pair:

| Staged DLL | SHA-256 |
|---|---|
| Engine | `D1690FFCD766A559BB7824A35BBC07A6C04B9A2E5B96202D7F0250299403F003` |
| Browser | `6A0464688651135952B699CA4BA975FFE9A2B7398E33211F4C7ADC9EBE4EDDF1` |

Cleanup verified both temporary payloads/profiles were removed, the game and MO2 were closed, Default was restored, the profiling override was disabled, and the shared runner lock was available. Production configuration remains releasedbg with profiling and smoke instrumentation disabled. The normal installed engine retained hash `482715C3A518E8FCBD890980AD86B7E90FA20515CADD7483FE08D6831D00CCB6`.

The migrated V2 binaries were built and tested offline; this focused comparison does **not** establish their fresh in-game acceptance. It also does not repeat the full Browser/reload matrix, audio/input checks or save/load qualification. No normal-mod deployment, push or package was performed by this task. This report is the second commit slice.
