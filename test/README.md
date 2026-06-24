# OSF Animation — offline test targets

All this test stuff is AI slop so :shrug:

Two `xmake` binary targets exercise the engine-independent code **without the game
or CommonLibSF**, so they run in CI / on any dev box. In-game behavior is covered
separately by the manual E2E suite in [../TESTSUITE.md](../TESTSUITE.md).

## `osf-tests` — unit tests

Pure-logic regression tests for the translation units that don't need the engine:
JSON pack/scene parsing, the unified matchmaker, the header-only utilities, the
frame clock, and the placement math.

```sh
xmake build osf-tests
xmake run   osf-tests          # exit code 0 = all passed
# or run the exe directly (no xmake "execv failed" wrapper on a failing run):
build/windows/x64/releasedbg/osf-tests.exe
```

### How it builds without the game

The real `SceneRegistry.cpp` and `Matchmaker.cpp` are compiled
as-is, but the target force-includes [`stubs/test_pch.h`](stubs/test_pch.h) instead
of `src/pch.h`. That pch substitutes [`stubs/re_stub.h`](stubs/re_stub.h) — minimal,
real-behavior stand-ins for the handful of `RE::*` types these TUs touch (actors,
keywords, races, the data handler) plus no-op `REX::*` loggers. The stubs are types
*we* own, so tests can construct actors with a sex/keywords/race and drive the full
filter-aware matchmaking binding path.

Form-refs (`"Plugin.esm|0xID"`) deliberately fail to resolve offline (no data
handler), which is exercised as the fail-soft scene-rejection case. The FormID
bit-layout itself is RE-sensitive and verified in-game (see `docs/RE.md`).

### Layout

- `framework/TestHarness.h` — dependency-free test runner (`OSF_TEST_CASE`, `CHECK*`).
- `unit/*.cpp` — the cases; `unit/main.cpp` chdirs to the fixtures and runs them.
- `fixtures/Data/OSF/*.json` — packs + scene graphs (valid, plus malformed cases for
  the diagnostics paths). The fixtures path is baked in at build time.

To add a case: drop an `OSF_TEST_CASE` in a new or existing `unit/*.cpp`
(self-registering, no wiring needed) and rebuild.

## `osf-import-test` — GLTF import harness

Runs the import pipeline on a `.glb` from the command line (no CLSF):

```sh
xmake build osf-import-test
build/windows/x64/releasedbg/osf-import-test.exe <file.glb> [animId]
```
