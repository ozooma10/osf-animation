**Registry/provider optimization and verification — October 22, 2026**

The Browser's derived clip provider now uses a compact input built directly from the pinned base registry. It avoids building, serializing and parsing presentation metadata that the provider does not use. Evidence, binary backups, source snapshots, controllers and comparison scripts are retained under [build/profiles/registry-provider-20261022](../build/profiles/registry-provider-20261022).

**Implementation.** Services 3.17 appends `RegisterClipSourceProvider`. Its schema-1 `ClipSourceSnapshot` includes every base scene ID, source filename, pack, tags, provider/library/curated flags and every inline clip address with its file, animation ID, availability, display path, species and authored duration. Scenes without clips remain present to reserve their IDs. The existing `RegisterClipSceneProvider` continues receiving the complete schema-3 catalog. The full `ReadCatalog` contract is unchanged.

[CatalogBuilder](../src/API/CatalogBuilder.cpp) shares the exact clip conversion between full and compact inputs. [ClipProviderBridge](../src/API/ClipProviderBridge.h) shares response validation and adds separate input, callback and decode timing scopes. Registry ownership, publication locking, whole-batch rejection and pinned old definitions remain unchanged. The updated Browser requires services 3.17; supported older consumers retain their existing service prefix.

[BrowserClipLibrary](../extensions/browser/src/API/BrowserClipLibrary.cpp) normalizes scene metadata once and selects the minimum source address per identity directly. It preserves filename/scene/node/stage/clip precedence, case-sensitive animation IDs, curated overrides, FNV IDs, collision suffixes, output order and launch defaults. Browser policy remains in the addon; the engine does not filter emote tags or select inspection defaults.

**Measurements.** The fresh control and final verification used the same 953-scene content set. Each closed-browser row covers two reloads; the visible check performs two successful Imports reloads.

| Measurement | Previous deployed pair | Final optimized pair |
|---|---:|---:|
| Provider rebuild | 183.400–200.566 ms | 53.864–56.172 ms |
| Complete registry reload | 294.494–322.595 ms | 163.587–169.934 ms |
| Provider tracked allocations per rebuild | 687,573 | 261,683 |
| Registry tracked allocations per reload | 1,332,159–1,332,162 | 906,270 |
| Visible Browser request → refreshed report | 496.401–497.417 ms | 374.796–393.030 ms |
| Visible main-thread registry reload, maximum | 292.721 ms | 177.546 ms |
| Closed-browser reload frame interval | 303.426–331.408 ms, one gap each | 172.173–178.493 ms, zero gaps |
| Full catalog read on worker | 158.029–171.013 ms | 156.760–171.178 ms |

Mean provider rebuild time fell **71.3%**, complete registry reload **46.0%**, and visible request-to-report time **22.7%**. Provider engine allocations fell **61.9%**. The matched 952-scene main matrix independently measured provider rebuild at **59.219–60.940 ms** and registry reload at **170.488–177.287 ms**, versus the preceding matrix's 180.279–190.044 ms and 290.390–300.300 ms.

The final compact input is approximately 2.38 MB. Input preparation took 29.5–31.8 ms in the final closed-browser captures, including 11.1–11.8 ms building source metadata. Provider invocation, including reply decoding, took about 24.3 ms; decoding alone was 0.86 ms. Trace thread attribution confirms that explicit Imports reload still runs on Game Main, while full `ReadCatalog` remains on the metadata worker. Reload remains a noticeable synchronous operation; this change reduces its work rather than eliminating the stall.

Full catalog JSON work still costs roughly 157–171 ms on its worker. The clip-availability sweep remains about 34–37 ms with 215,238 tracked allocations. These are the remaining measured targets.

**Regression checks.** The final full matrix passed idle and profiler off/stats/trace modes; repeated one/two/eight-actor AF playback; cold/warm GLB; transitions; cues/audio; equipment/control; additive poses; two managed actors with sixteen unmanaged actors; two content reloads; quiescent save/load; and final idle. Final supplemental checks passed camera playback, positive/negative query batches, equipment and two additional eight-actor repetitions.

Several initial steady windows were 1–6 microseconds above the earlier session, including one two-actor window at +21%. Fresh controls using the backed-up previous binaries were therefore followed by the same targeted case order and durations on the final build. Mean instrumented OSF time per hook:

| Fresh playback comparison | Previous pair | Final pair |
|---|---:|---:|
| 1 AF actor | 0.01485 ms | 0.01361 ms |
| 2 AF actors | 0.02615 ms | 0.02490 ms |
| Transitions | 0.03301 ms | 0.03176 ms |
| Effects | 0.03630 ms | 0.03405 ms |
| Warm GLB | 0.02763 ms | 0.02649 ms |
| Actor density | 0.05410 ms | 0.05238 ms |

No sustained playback slowdown was reproduced. The two-actor p99 was slightly higher, 0.0408 versus 0.0396 ms; other targeted p99 values were lower. Eight-actor final supplemental means were 0.10697–0.10722 ms versus the fresh control's 0.11424–0.11737 ms. Camera/equipment comparisons also passed, but their maintenance cost varied with camera state (about 1.2 versus 7 microseconds), so those differences are not claimed as optimization gains. These bounded repetitions do not establish a universal speedup for unchanged playback. `steady-comparison.csv` retains all higher and lower windows, p99, normalized elapsed time per second and pose/camera costs.

Visible Browser acceptance passed with **179 catalog entries, 1,077 library entries, 38 files and 953 authored scenes**. Both reloads preserved the entire sorted ID sets, and one overlapping reload was rejected. Control and final fingerprints match exactly: catalog `53c9f9b8e0322db8`, library `5230e3c813f44e93`. The [final captured view](../build/profiles/registry-provider-20261022/runs/20261022-171804-137-ComprehensiveProfiling/browser-catalog-refresh.png) was inspected and shows populated results and the successful reload confirmation.

The 15 native suites passed, including direct compact/full metadata equivalence, both production bridge input modes, malformed/missing/repeated provider responses, source-order permutations, animation-ID case, authored-ID collisions, curated precedence, availability and filtering. Existing provider tests continue to cover atomic batch rejection, rebuild/removal and old-definition lifetime. The Browser boundary check passed. The Browser view's 166 tests, TypeScript check and production build passed after installing the locked dependencies.

**Profiler failure and repair.** One focused run timed out waiting for the equipment-start dump. The stats file and log showed completion at `16:50:42.110Z`; published status remained at sequence 25 with `busy=true`, while `status.json.tmp` held sequence 26 with `busy=false` and the completed dump. These files and the failed result are retained. Windows replacement checks confirmed that an open reader can block replacement, including with delete sharing in this environment. The command reader now permits delete sharing, and the development-only host retains a dirty status flag and retries publication from its normal poll without sleeping on the frame thread.

The final full run deliberately held the published status file open until a dump completed. It verified that replacement was blocked, then closed the reader and verified publication of that same successful dump on a later poll. `blocked-status.json` and `recovered-status.json` retain the evidence. The failed run remains failed; it is not counted as a complete matrix.

The first optimization pilot retained the full provider input and filtered fields during parsing. It passed functional Browser checks but did not improve rebuild performance, so it was superseded. A per-read lexical metadata cache also added allocations on this dataset and was removed. Pilot/intermediate binaries and logs remain separate from the final build.

**Evidence and deployment.** Seven launches retained **186 stats captures and 28 traces**, including **105 stats captures and 16 traces on the final binaries**. All captures report zero wrapped rings, torn events, dropped descriptors and dropped threads.

| Run | Purpose | Outcome / captures |
|---|---|---|
| `20261022-164057-205-ComprehensiveProfiling` | Superseded full-input filtering pilot | Functional pass; 7 stats, 3 traces |
| `20261022-164331-050-ComprehensiveProfiling` | Previous-pair focused/reload/Browser control | Passed; 20 stats, 4 traces |
| `20261022-164858-735-ComprehensiveProfiling` | First compact-input focused run | Status-publication failure; 9 stats, 1 trace retained |
| `20261022-165254-824-ComprehensiveProfiling` | Compact-input repeat before final cache removal/status retry | Passed; 20 stats, 4 traces |
| `20261022-165902-020-ComprehensiveProfiling` | Final full matrix and forced status-retry proof | Passed; 64 stats, 12 traces |
| `20261022-171300-311-ComprehensiveProfiling` | Previous-pair targeted playback control | Passed; 25 stats |
| `20261022-171804-137-ComprehensiveProfiling` | Final targeted playback, supplemental cases and Browser | Passed; 41 stats, 4 traces |

`provider-comparison.json`, `steady-comparison.csv` and `trace-threads.json` are reproduced by the adjacent Python scripts. Each run also has its own scope/window/trace analysis, logs, workload source and binary identity. `source-manifest.json` matches the retained final native sources and tests. Production was built/deployed in `releasedbg` with `profiling=n` and `animation_smoke=n`; the profiling build was deployed only to its separate override mod. Final built/deployed hashes match:

- Engine: `E92870EE57FF71273F8B9B6FA26646948292AA953A2698FED1856149C0EE032D`
- Browser: `6AC6C10BFC6CC5F80CB4CDB5618BE56F8DDC6877345451B68B5759EF8158A65E`
- Profiling override: `893C3744FDF5C0E03BCA355704E62DADAA8796D944BEDF3D1E70391204188EAE`

[Final verification](../build/profiles/registry-provider-20261022/final-verification.json) confirms the normal engine has no profiling marker, the override is disabled in Default, Default is selected, all seven temporary-profile journals report removal, and no temporary mod/profile or Starfield/MO2 process remains.

Live Browser acceptance used the installed view bundle (`index-wVElcjZH.js`) with the updated native DLL. The separate current-source UI verification produced `index-BuXpY8Gr.js`; native xmake deployment does not publish Vite output. Both manifests are retained separately. The private acceptance script was confined to disposable view copies.

The comparisons use the same private test cell, content fixtures, AF/GLB inputs and profiler modes as the preceding session. Main matrices have 952 authored scenes from 38 files; focused/browser matrices add the camera fixture for 953. The fresh control is the previously deployed catalog-refresh-optimized pair, not the older pre-catalog-refresh baseline. Profiling allocation counts cover the instrumented engine module; they do not count allocations performed inside the separately built Browser DLL. Scope durations are elapsed time and nested scopes must not be added together.

Frame-hook intervals are not rendered FPS. The profiler classifies intervals over 250 ms as gaps and excludes them from normal frame-time maxima. Gap counts/durations therefore accompany the reload comparison; inspecting only `frameMs.max` would misrepresent the improvement.

Physical controller/camera interaction, audible output, populated-city load and large persistence payloads were not qualified. The audio fixture is silent, and save/load uses a quiescent scene state and empty client records. The live Browser checks use its public bridge in a private view copy and include a captured, inspected rendered view; they do not exercise physical input. No commit, push or package was made.
