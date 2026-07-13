# SceneLauncher Extraction — Plan

## Goal

Extract the matchmake-and-start funnel from `src/Papyrus/OSFScript.cpp` into a new
`src/Scene/SceneLauncher.{h,cpp}` so that **native code can launch scenes the same way
Papyrus does**. This is step 1 of the "foundations" work for the launch feature set:
the upcoming `HotkeyService` (`toggleSceneTags:player.sit` etc.) and, later, the
Ambient Director both need to call this funnel from C++, and today it is trapped in
the anonymous namespace of the Papyrus bindings file.

**This is a pure mechanical refactor — zero behavior change.** Same log lines, same
HUD messages, same return values, same matchmaking. If a choice arises between
"cleaner" and "byte-identical behavior", pick byte-identical.

## Constraints

- Read `AGENTS.md` first and follow its rules (log tags, style, recompile notes).
- The build chain is Windows-only (xmake + MSVC + CommonLibSF); this dev box is a Mac.
  **You cannot compile.** Compensate: keep the refactor mechanical, re-read the full
  diff for correctness (includes, namespaces, moved defaults), and grep every moved
  symbol to confirm all call sites still resolve. The user compiles and tests in-game.
- `xmake.lua` globs `src/**.cpp` — new files are picked up automatically, no build edit.
- Do not touch `.psc` / `.pex` files — no Papyrus signature changes in this task.

## What moves (all currently in the anonymous namespace of `src/Papyrus/OSFScript.cpp`)

| Symbol | Current location | Notes |
|---|---|---|
| `SceneOpts` struct | ~L69–82 | → header, e.g. `Scene::LaunchOpts`. Keep every field, default, and the "keep in sync with OSFTypes.psc SceneOptions" comment. |
| `OptHeadingRad` | ~L144 | Depends only on `SceneOpts` + `Util::kDegToRadF`. |
| `MakeAnchor` | ~L150 | Thin wrapper over `Scene::MakeAnchorAt`. |
| `kLoopScaleMax` + `MakeOverrides` | ~L162–193 | Tri-state → `optional<bool>` mapping + LoopScale sanitization. Keep the comments. |
| `StartCandidate` | ~L218–230 | Applies the matchmade slot order and starts anchored/co-located. |
| `StartMatched` | ~L486–516 | The funnel body. → public entry point, e.g. `Scene::LaunchMatched(actors, query, opts, overrides, logTag, anchorMode = kAllow)`. |

The local `ResolveSceneAnchor` wrapper (~L156) is just
`Scene::ResolveSceneAnchor(id, opts.anchor, OptHeadingRad(opts), /*emitHud*/ true)` —
in the moved code call `Scene::ResolveSceneAnchor` directly; keep the tiny wrapper in
OSFScript.cpp only if other natives still use it (check `StartScene` ~L445 — it does;
either keep it there or expose the same convenience from SceneLauncher — pick one,
don't duplicate).

## What stays in `OSFScript.cpp`

- `ReadSceneOptions` (~L85–141): Papyrus `structure_wrapper` unpacking is
  binding-layer work. It now returns the shared `Scene::LaunchOpts`.
- `ResolveStageEntryNode`, all natives, registration. `StartSceneByTags`,
  `StartSceneByTagsQuery`, `StartSceneAtAnchor` become thin wrappers:
  build the `TagQuery` / read options / call `Scene::LaunchMatched`.
- **Careful:** `MakeAnchor` + `MakeOverrides` are also used by natives that do NOT go
  through the funnel — the `StartFromPlan` family (~L726, 748, 778, 813, 835) and
  `StartScene` (~L449). After the move these call sites use the `Scene::`-qualified
  versions via the new header. Grep `MakeAnchor(opts)` / `MakeOverrides(opts)` and fix
  every site.

## Logging detail (behavior-preserving)

`StartMatched` currently logs `"[Papyrus] {logTag}: ..."` with logTag like
`"OSF.StartSceneByTags"`. The moved funnel must not hardcode `[Papyrus]` (native
callers will be `[Hotkey]`, `[Ambient]`). Change the format strings to `"{}: ..."` and
have the Papyrus wrappers pass `"[Papyrus] OSF.StartSceneByTags"` etc., so today's
log output stays byte-identical. Document this convention in the `LaunchMatched`
doc comment.

## New files — shape

`src/Scene/SceneLauncher.h` (namespace `OSF::Scene`, match AnchorResolve.h style):

```cpp
// Per-start options shared by every launch path (Papyrus natives, hotkeys, director).
// Field semantics/defaults mirror OSFTypes.psc SceneOptions — keep in sync.
struct LaunchOpts { ... };                      // the moved SceneOpts

std::optional<float>              OptHeadingRad(const LaunchOpts&);
SceneRuntime::AnchorOverride      MakeAnchor(const LaunchOpts&);
SceneRuntime::StartOverrides      MakeOverrides(const LaunchOpts&);

// Validate actors, matchmake the query (priority tier + weighted-random, anchor
// filtering), enforce the pick's anchor requirement, start with the matchmade
// binding. Returns the scene handle (0 = failed). a_logTag carries the full log
// prefix, e.g. "[Papyrus] OSF.StartSceneByTags".
std::int32_t LaunchMatched(const std::vector<RE::Actor*>& a_actors,
    const Matchmaking::TagQuery& a_query, const LaunchOpts& a_opts,
    const SceneRuntime::StartOverrides& a_over, const char* a_logTag,
    Matchmaking::AnchorMode a_mode = Matchmaking::AnchorMode::kAllow);
```

`SceneLauncher.cpp` includes: `Matchmaking/Matchmaker.h`, `Scene/AnchorResolve.h`,
`Scene/SceneRuntime.h`, `UI/HudMessage.h`, `Util/Math.h` (all already used from
comparable layers — AnchorResolve already emits HUD messages, so no layering concern).
`StartCandidate` can stay file-local (anonymous namespace) in SceneLauncher.cpp unless
a clean reason emerges to expose it.

## Explicitly out of scope (do NOT do now)

- `src/API/OSFSceneAPI.cpp` ~L25 keeps a **hand-synced mirror** of
  MakeOverrides/MakeAnchor for the native API's `OSFStartOptions` ("kept in lockstep").
  Unifying it means mapping `OSFStartOptions` → `LaunchOpts` — a real change to the
  stable API surface's code path. Leave it. Update its lockstep comment to point at
  `src/Scene/SceneLauncher.h` as the new home of the canonical version.
- No new natives, no hotkeys, no `GetSceneId` — separate tasks.

## Acceptance criteria

1. `src/Scene/SceneLauncher.{h,cpp}` exist; funnel symbols live there; OSFScript.cpp's
   anonymous namespace no longer defines SceneOpts/OptHeadingRad/MakeAnchor/
   MakeOverrides/StartCandidate/StartMatched.
2. All Papyrus natives compile against the shared header (verified by grep — every
   former call site updated; no orphaned references).
3. Log/HUD output paths are string-identical for the Papyrus routes (spot-check the
   format strings against `git diff`).
4. OSFSceneAPI.cpp lockstep comment updated; no other files changed.
5. CHANGELOG: no entry needed (internal refactor) unless AGENTS.md says otherwise.

## Verification (user, on Windows)

- Build via xmake; fix any compile errors by reporting them back (do not guess-edit
  unrelated code).
- In-game smoke: `cgf "OSFTest.Furniture2"` and any StartSceneByTags-driven test
  (Data Slate → browser launch a scene) — matchmade starts, anchored starts, and the
  "no matching animation" HUD error all behave as before.
