# OSF Animation — API review (pre-beta)

A pass over the public Papyrus surface (`OSF.psc` / `OSFCompat.psc`) ahead of the v0.x beta, to make
the API "as good as possible" before modders build on it. Compared against `SCENE_DESIGN.md` Part 1
(the intended 1.0 contract). Goal: settle the surface now so the eventual 1.0 freeze is low-churn.

> Findings touching shared C++ (`OSFScript.cpp`, `SceneRegistry.*`, `OSF.psc`) are a **punch-list for
> the Phase C agent** (which is actively editing those files), not parallel edits. See §5.

## 1. Stability promise (state this in the beta)

- **Tier 0 primitives are STABLE** (`Play`/`Stop`/`SetSpeed`/`GetSpeed`/`SetAnchor`/`ClearAnchor`/
  `Sync`/`PlaySequence`/`GetCurrentAnimation`/`IsPlaying`). Safe to build on.
- **The scene API is BETA, may refine.** It was reshaped during implementation (e.g. `StartScene`
  returned `bool` → now an `int` handle). Don't promise the never-break guarantee until 1.0.

## 2. Freeze-risk items (expensive to change once shipped — prioritize)

### 2.1 The callback payload struct `OSFEvent:SceneEvent`
Frozen with the ABI. The marshaller maps **by member name** (it iterates `varNameIndexMap`), so:
- **Appending** fields is forward-compatible (old callbacks ignore them). ✅
- **Renaming / removing** a field is a hard break. 🔴
- **Action before beta:** ratify the field-name set as final. Current members: `sceneHandle`,
  `eventType`, `node`, `edge`, `cue`, `actionType`, `actorRef`, `role`, `loopIndex`, `time`,
  `anchor`, `result`. Confirm with the Phase C agent that `actorRef`/`role`/`loopIndex` get filled
  (real actor marshalling) or are documented as reserved — either way the *names* must be final.

### 2.2 Event / result constants as functions
`OSF.EVENT_NODE_ENTER()` etc. — the `()` is forced (a `Native Hidden` script can't expose readable
properties on the type). It works, but it's the kind of ergonomic wart modders bake into their code.
- **Option (recommended):** ship a small **non-Native** companion script, e.g. `OSFConst`, with
  `Int Property EVENT_NODE_ENTER = 1 AutoReadOnly` … so consumers write `OSFConst.EVENT_NODE_ENTER`.
  Keep the `OSF.X()` functions too (don't break the existing form). Decide before beta so the
  cleaner form is the documented one.

## 3. Gaps vs. the spec (good framework DX, missing today)

| Item | Spec | Status | Recommendation |
|---|---|---|---|
| `ValidateScene(asId)` | §1.6 | not bound | Add — "lint my scene file without running it" is core modder DX; the parser already produces the errors (`GetSceneLoadErrors` infra). |
| `GetSceneValidationErrors(asId)` | §1.6 | not bound | Add alongside the above. |
| `HasFeature` capability names | §1.2 | only `scenes`/`playback`/`sync`/`anchor` | Extend to `cues`/`actions`/`callbacks` (+ Phase-C lanes) so consumers can feature-gate the merged engine. |
| `ReloadContent` (alias of `ReloadPacks`) | §1.6 | not bound | Trivial alias; optional. |

## 4. Divergences to ratify (implemented, but not in the contract)

- **`GetSceneStageForActor` / `SetSceneStageForActor`** — added for the SAF/`PlaySequence` path (a
  `PlaySequence` graph has no scene handle). They're consistent with the `GetSceneForActor` /
  `StopSceneForActor` convention and genuinely useful. **Promote them into `SCENE_DESIGN.md` §1.2 +
  `docs/API.md`** so they're official, not accidental.
- **ID-resolution prefixes** (`scene:` / `anim:`, scene-first) — implemented in `StartScene` and
  stripped in `StartSceneRoles`. Confirm the behavior is consistent across all `Start*` and documented.

## 5. Punch-list for the Phase C agent (shared-file edits)

Hand these to the track that owns `OSFScript.cpp` / `SceneRegistry.*` / `OSF.psc`:

1. Bind `ValidateScene` + `GetSceneValidationErrors`.
2. Extend `HasFeature` with `cues`/`actions`/`callbacks` (+ new lane names).
3. Add `GetSceneStageForActor`/`SetSceneStageForActor` to `OSF.psc`'s doc comments + `docs/API.md`.
4. Lock the `OSFEvent:SceneEvent` field names; fill or reserve `actorRef`/`role`/`loopIndex`.
5. Decide unimplemented-`osf.*`-action behavior: load-time warning vs. silent log (recommend warning).
6. (Optional) `OSFConst` constants script; `ReloadContent` alias.

## 6. What's already good (no action)

- Sentinel discipline is consistent and documented (handle `0` = fail, stage `-1`, etc.).
- Handles are generational (`[generation|slot]`) — stale handles read invalid, never collide.
- Actor exclusivity keeps `GetSceneForActor` single-valued and the auto-end/cue resolvers unambiguous.
- The async callback transport (struct payload via `DispatchMethodCall`) is settled and proven.
- Load-safety (handles die on `StopAll`) and the co-load guard are in place.
