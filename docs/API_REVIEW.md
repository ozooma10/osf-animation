# OSF Animation — API review (pre-beta)

A pass over the public Papyrus surface (`OSF.psc` / `OSFCompat.psc`) ahead of the v0.x beta, to make
the API "as good as possible" before modders build on it. Compared against `SCENE_DESIGN.md` Part 1
(the intended 1.0 contract). Goal: settle the surface now so the eventual 1.0 freeze is low-churn.

> Status update: the beta-facing API polish from this review has landed. This file is now the audit
> record for what was checked, what was resolved, and the one optional alias left as fast-follow.

## 1. Stability promise (state this in the beta)

- **Tier 0 primitives are STABLE** (`Play`/`Stop`/`SetSpeed`/`GetSpeed`/`SetAnchor`/`ClearAnchor`/
  `Sync`/`PlaySequence`/`GetCurrentAnimation`/`IsPlaying`). Safe to build on.
- **The scene API is BETA, may refine.** It was reshaped during implementation (e.g. `StartScene`
  returned `bool` → now an `int` handle). Don't promise the never-break guarantee until 1.0.

## 2. Freeze-risk items (expensive to change once shipped — prioritize)

### 2.1 The callback payload struct `OSFEvent:SceneEvent` — ✅ FIELD NAMES LOCKED
Frozen with the ABI. The marshaller maps **by member name** (it iterates `varNameIndexMap`), so:
- **Appending** fields is forward-compatible (old callbacks ignore them). ✅
- **Renaming / removing** a field is a hard break. 🔴
- **Resolved:** the field-name set is final: `sceneHandle`, `eventType`, `node`, `edge`, `cue`,
  `actionType`, `actorRef`, `role`, `loopIndex`, `time`, `anchor`, `result`. `actorRef` is packed as
  a real Actor object for role-bearing `EVENT_ACTION` payloads; other events leave it `None`.
  `loopIndex` is present and currently reserved/defaults to `-1` until loop-index reporting is wired
  or explicitly deferred in the 1.0 contract.

### 2.2 Event / result constants as functions — ✅ RESOLVED (keep functions; WONTFIX the companion)
`OSF.EVENT_NODE_ENTER()` etc. — the `()` is forced (a `Native Hidden` script can't expose readable
properties on the type). It works, but it's the kind of ergonomic wart modders bake into their code.
- **Investigated & rejected:** the recommended "non-Native `OSFConst` companion with `Int Property
  EVENT_NODE_ENTER = 1 AutoReadOnly`" **does not deliver cleaner syntax** — Papyrus rejects reading a
  property on a type name (`a property cannot be used directly on a type, it must be used on a
  variable`, compiler-verified 2026-06-17). `OSFConst.EVENT_NODE_ENTER` only compiles if the consumer
  holds an `OSFConst` *instance* (a CK-filled property or a cast), which is *more* friction than the
  `()`, not less.
- **Decision:** the global getter functions are the only zero-friction type-level constant access in
  Papyrus; the `()` is the language tax. Keep `OSF.X()` as the canonical, documented form. No
  companion script.

## 3. Gaps vs. the spec

| Item | Spec | Status | Recommendation |
|---|---|---|---|
| `ValidateScene(asId)` | §1.6 | ✅ bound | Landed; returns whether the scene id loaded successfully. |
| `GetSceneValidationErrors(asId)` | §1.6 | ✅ bound | Landed; filters the load-error stream for the requested id. |
| `HasFeature` capability names | §1.2 | ✅ extended | Now recognizes `cues`/`actions`/`sound`/`camera`/`callbacks`/`weapon` through the aggregate engine gate. |
| `ReloadContent` (alias of `ReloadPacks`) | §1.6 | optional, not bound | Nice-to-have naming alias only; not a beta gate. |

## 4. Divergences to ratify (implemented, but not in the contract)

- **`GetSceneStageForActor` / `SetSceneStageForActor`** — ✅ ratified. Added for the SAF/
  `PlaySequence` path (a `PlaySequence` graph has no scene handle), documented in `OSF.psc` and
  `docs/API.md`.
- **ID-resolution prefixes** (`scene:` / `anim:`, scene-first) — ✅ documented and used by the
  `Start*` paths.

## 5. Punch-list status

Beta-facing edits:

1. ✅ Bind `ValidateScene` + `GetSceneValidationErrors`.
2. ✅ Extend `HasFeature` with `cues`/`actions`/`sound`/`camera`/`callbacks`/`weapon`.
3. ✅ Add `GetSceneStageForActor`/`SetSceneStageForActor` to `OSF.psc`'s doc comments + `docs/API.md`.
4. ✅ Lock the `OSFEvent:SceneEvent` field names; pack `actorRef` for role-bearing action events;
   keep `loopIndex` reserved/defaulted for now.
5. ✅ Decide unimplemented-`osf.*`-action behavior: unknown `osf.*` rejects the scene at load.
6. ✅ Reject `OSFConst` as a companion constants script; `OSF.EVENT_X()` / `OSF.RESULT_X()` remain
   canonical. Optional fast-follow: `ReloadContent` alias.

## 6. What's already good (no action)

- Sentinel discipline is consistent and documented (handle `0` = fail, stage `-1`, etc.).
- Handles are generational (`[generation|slot]`) — stale handles read invalid, never collide.
- Actor exclusivity keeps `GetSceneForActor` single-valued and the auto-end/cue resolvers unambiguous.
- The async callback transport (struct payload via `DispatchMethodCall`) is settled and proven.
- Load-safety (handles die on `StopAll`) and the co-load guard are in place.
