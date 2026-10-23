**Full catalog JSON serialization — October 22, 2026**

The engine now streams schema-3 catalog JSON from its owned catalog values, eliminating the producer's full temporary JSON tree. Evidence, previous binaries, source snapshots, controllers and comparisons are retained under [build/profiles/catalog-json-20261022](../build/profiles/catalog-json-20261022).

**Implementation.** [CatalogJson](../src/API/CatalogJson.h) writes objects and arrays to one owned string. `OSFCatalog.h` declares each record's fields once for both the existing JSON converters and the streaming visitor. One reusable JSON string scalar and the pinned nlohmann 3.11.3 serializer preserve escaping, malformed UTF-8 replacement, numeric formatting and non-finite-number handling. JSON object members now follow declaration order; all values, fields and array ordering are preserved. No shared serializer state or cross-read cache is introduced.

`ReadCatalog` still pins the immutable registry, builds metadata and invokes the borrowed-text callback synchronously on the caller's thread. The callback's text remains valid for the entire callback. The legacy full-catalog provider path uses the same serializer. Compact schema-1 provider input, Browser parsing, registry publication and playback are unchanged. Services remain 3.17 and the full catalog remains schema 3.

**Fresh matched comparison.** Both focused runs used the same 953 authored scenes, 38 files, AF/GLB fixtures, installed Browser view and scenario order. The control used the backed-up registry/provider-optimized pair from the preceding session. Each closed-browser row covers two reloads; the visible check performs two successful Imports reloads.

| Measurement | Previous deployed pair | Streaming serializer |
|---|---:|---:|
| Full catalog read, including inline Browser parse | 153.187–164.118 ms | 89.857–99.973 ms |
| Engine allocations per full read | 736,548–736,550 | 223,982–223,984 |
| Visible request → refreshed report | 374.169–376.219 ms | 307.816–312.603 ms |
| Complete registry reload | 162.083–169.121 ms | 163.387–165.147 ms |
| Closed-browser reload frame interval, maximum | 170.767–177.733 ms | 172.319–174.030 ms |
| Reload frame gaps over 250 ms | 0 | 0 |

Mean full-read elapsed time fell **40.2%**, tracked engine allocations **69.6%**, and visible request-to-report time **17.3%**. Full documents remain approximately 6.13 MB; they contain the same schema rather than a reduced projection. Small live byte-count differences arise from per-load diagnostic timings. Unit comparisons require identical compact payload sizes for identical input values.

New phase scopes separate metadata construction (24.7–25.2 ms), serialization (17.9–18.2 ms, **28 allocations**) and the inline Browser JSON parse (43.3–48.7 ms) in the closed-browser captures. Visible reloads measured serialization at 19.5 ms and parsing at 43.5–43.9 ms. Trace attribution confirms these full-read phases run on the Browser metadata worker. Explicit Imports registry reload remains on Game Main and takes about 164–168 ms in the new visible capture. This optimization shortens the following metadata refresh; it does not eliminate the synchronous registry stall.

`API.ReadCatalog` includes callback time and local cleanup. It does not include the Browser's later typed conversion/decorating or UI delivery; the visible request-to-report measurement covers that longer path. Allocation hooks count the instrumented engine DLL only. Browser allocation counts are unavailable, and a zero count in the callback scope does not mean the Browser parser allocates nothing. Allocation bytes are cumulative, not peak resident memory. Nested elapsed scopes must not be summed.

**Regression checks.** The fresh focused pair passed camera playback, positive/negative batches of 100 queries, equipment/control and two eight-actor repetitions. Mean instrumented OSF time per hook:

| Focused window | Previous | New | Previous p99 | New p99 |
|---|---:|---:|---:|---:|
| Camera | 0.026313 ms | 0.025234 ms | 0.0470 ms | 0.0384 ms |
| Equipment | 0.027911 ms | 0.027794 ms | 0.0399 ms | 0.0425 ms |
| Eight actors, repeat 1 | 0.110438 ms | 0.108631 ms | 0.1673 ms | 0.1631 ms |
| Eight actors, repeat 2 | 0.110735 ms | 0.111824 ms | 0.1657 ms | 0.1795 ms |

The higher tails and +1.0% second eight-actor mean are retained in the comparison; these bounded repetitions do not establish a universal playback speedup.

The complete matrix passed idle and profiler off/stats/trace modes; repeated one/two/eight-actor AF playback; cold/warm GLB; transitions; cues/audio; equipment/control; additive poses; two managed actors with sixteen unmanaged actors; two registry reloads; quiescent save/load; and final idle. This matrix has 952 authored scenes, omitting the focused camera fixture. Full reads independently fell from the preceding matrix's 153.714–170.035 ms to **88.303–102.952 ms**, with engine allocations falling from 736,334–736,336 to 223,904–223,906. Final-idle mean rose from 0.001456 to 0.001547 ms (+0.091 microseconds); its p99 rose from 0.0029 to 0.0033 ms.

No functional regression or consistent playback slowdown was observed. Against the preceding full matrix, the first eight-actor mean fell 3.8%, while the second rose 3.2% (p99 0.1667 → 0.2080 ms). The additional eight-actor stats windows rose 1.9–3.4% in mean but had lower p99 values; both trace windows were lower. Additive p99 rose from 0.0751 to 0.0802 ms while its mean fell 1.2%. Other steady playback means were lower. These mixed microsecond-scale changes are recorded as variability, not claimed as serialization benefits. `steady-comparison.csv` retains every matched window, including higher tails, normalized elapsed time per second and pose/camera costs. The focused control is fresh; the full-matrix control is the preceding session on the exact backed-up binary pair.

Visible Browser acceptance passed with **179 catalog entries, 1,077 library entries, 38 files and 953 authored scenes**. Two reloads preserved complete sorted ID sets, and an overlapping reload was rejected. Control and optimized fingerprints match: catalog `53c9f9b8e0322db8`, library `5230e3c813f44e93`. Both captured views were inspected; the [optimized view](../build/profiles/catalog-json-20261022/runs/20261022-175349-932-ComprehensiveProfiling/browser-catalog-refresh.png) shows populated results and the successful reload confirmation.

All **15 native suites** passed, including full-document equivalence across nested records, diagnostics, empty arrays, all isolated byte values, control characters, quotes/backslashes, valid Unicode, malformed/truncated UTF-8, long escaped strings, integer limits, negative zero, non-finite floats, 512 deterministic floating-point bit patterns and output ownership. Existing bridge tests cover both provider schemas and malformed/missing/repeated replies. Browser boundary checks passed. The Browser's **166 tests**, TypeScript check and production build passed with locked dependencies.

The first compile attempt called a private nlohmann helper and failed. It was corrected to use the serializer's public `dump` entry with reusable scalar storage before runtime testing; the initial failure log is retained separately.

**Evidence and deployment.**

Three launches retained **103 stats captures and 20 traces**, including **83 stats captures and 16 traces on the final binaries**. Every capture reports zero wrapped rings, torn events, dropped descriptors and dropped threads.

| Run | Purpose | Outcome / captures |
|---|---|---|
| `20261022-174954-486-ComprehensiveProfiling` | Previous-pair focused/reload/Browser control | Passed; 20 stats, 4 traces |
| `20261022-175349-932-ComprehensiveProfiling` | Optimized focused/reload/Browser verification | Passed; 20 stats, 4 traces |
| `20261022-175748-652-ComprehensiveProfiling` | Optimized full matrix | Passed; 63 stats, 12 traces |

`catalog-comparison.json`, `steady-comparison.csv` and `trace-threads.json` are reproduced by the adjacent scripts. Per-run scope/window/trace analysis, workload sources, logs and binary identities are retained. `source-manifest.json` matches the retained tested native sources. Production was built/deployed in `releasedbg` with `profiling=n` and `animation_smoke=n`; the profiler was deployed only to its separate override mod. Final built/deployed hashes match:

- Engine: `ADB20395CAB4888F6F45BA68343C409E7700BD9C9622C9C2B4BE254693653AF2`
- Browser: `4B225E50DFE3BF249C1AB37FE40D8B4FDC1213C7646D9CFC0739CAB61330F5B0`
- Profiling override: `267DECBD24840DA0B7CB445E9799910F8950664586DFDDE897EF767809C3D28A`

[Final verification](../build/profiles/catalog-json-20261022/final-verification.json) confirms that the normal engine has no profiling marker, Default is selected, the override is disabled, every temporary-profile journal reports removal, and no temporary mod/profile or Starfield/MO2 process remains. The current source matches the tested-source manifest, and the Git whitespace check passed.

The installed Browser bundle (`index-wVElcjZH.js`) was used for live acceptance. The separate current-source UI verification produced `index-BuXpY8Gr.js`; native xmake deployment does not publish Vite output. Final verification records these manifests separately. Private probe scripts were confined to disposable copies.

Frame-hook intervals are not rendered FPS. Intervals over 250 ms are recorded as gaps and excluded from normal frame-time maxima, so gap counts accompany the reload comparison. Physical input, audible output, populated-city load and large persistence payloads remain outside this qualification. The audio fixture is silent; save/load uses quiescent state and empty client records. No commit, push or package was made.
