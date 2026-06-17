# Migrating from SAF to OSF Animation

OSF Animation is a **drop-in replacement** for SAF (Starfield Animation Framework). Existing SAF
mods keep working through a compatibility shim — you install OSF *instead of* SAF, and most SAF
content runs unchanged.

This is the v0.x **beta** headline. The common SAF surface (play, paired scenes, sync, sequences,
player locks, crosshair) is fully covered; a handful of advanced features are not yet bridged (see
[Known gaps](#known-gaps)).

## Installing (it replaces SAF)

OSF and SAF **cannot run together** — they both stamp the actor rig, so OSF detects a co-loaded SAF
(or NAFSF) at startup and shows a warning. To migrate:

1. **Disable / uninstall SAF** (and NAFSF, if present).
2. Install **OSF Animation** + its requirements: Starfield **1.16.244.0**, matching **SFSE**,
   **Address Library for SFSE Plugins**.
3. Install your SAF content mods as before — they call `SAF.*` / `SAFScript.*`, which OSF provides.

> SAF content calls the `SAF` and `SAFScript` Papyrus scripts. OSF ships both as forwarding shims,
> so no recompile of SAF content is needed.

## Coverage

The two SAF scripts OSF provides:

- **`SAFScript`** — the global-function API (`SAFScript.PlayOnActor(...)`, etc.).
- **`SAF`** — the original struct-based API (`SequencePhase`, `PlaySceneLocked`, furniture variants).

Status: ✅ full · 🟡 works with a caveat · ⚪ intentional no-op (OSF handles it automatically) ·
❌ SHIM-GAP (capability not bridged yet).

| SAF area | Functions | Status | Notes |
|---|---|---|---|
| Readiness | `Ping` | ✅ | → `OSF.IsReady` |
| Single-actor playback | `PlayOnActor`, `PlayAnimationOnce`, `PlayOnPlayer`, `PlayOnActors`, `PlayOnActorLocked`, `PlayOnPlayerLocked` | ✅ | → `OSF.Play` (+ speed, + player lock) |
| Paired / multi scenes | `PlayScene`, `PlaySceneSeparate`, `PlaySceneLocked*`, `PlaySceneSeparate*`, `PlaySceneWithApproach*` | ✅ | → `OSF.StartSceneFiles` (co-locates + syncs actor2); approach loop is plain Papyrus |
| Furniture variants | `PlayOnActorAtFurniture`, `PlaySceneAtFurniture*` | ✅ | `MoveTo` + settle, then OSF playback |
| Stop | `StopAnimation` | ✅ | → `OSF.Stop` (solo) / `OSF.StopSceneForActor` (scene) |
| Sync | `SyncGraphs`, `SyncAnimations` | ✅ | → `OSF.Sync` (shared-clock frame-lock) |
| Sequences | `StartSequence`, `AdvanceSequence`, `SetSequencePhase`, `GetSequencePhase` | ✅ | → `OSF.PlaySequence` + the actor-keyed stage methods |
| Speed | `SetAnimationSpeed`, `GetAnimationSpeed` | ✅ | SAF percent ↔ OSF multiplier (100 = 1.0×) |
| Current anim | `GetCurrentAnimation` | 🟡 | returns the **full Data-relative path**, not the bare id (Papyrus can't strip the root) |
| Player lock | `LockActorForAnimation*`, `UnlockActorAfterAnimation*` | ✅ | player participant → control + camera lock (`OSFCompat`) |
| Crosshair pick | `GetCrosshairRef`, `GetCrosshairActor`, `PickActorFromCrosshair`, `PickPairActorFromCrosshair`, `FindActorNearCrosshair` | 🟡 | native engine crosshair target; a Papyrus heading-cone search is the fallback when the reticle isn't pixel-on a listed actor |
| Position locking | `SetPositionLocked`, `SetGraphControlsPosition` | ⚪ | no-op — OSF anchors/pins scene participants automatically |
| Absolute positioning | `SetActorPosition`, `MatchActorTransform` | ❌ | SHIM-GAP — OSF pins via the scene anchor; an absolute `SetPosition` would fight the compose-root pin |
| Blend-graph variables | `SetBlendGraphVariable`, `GetBlendGraphVariable` | ❌ | SHIM-GAP — OSF plays GLB clips, it has no blend-graph variable system |
| Retro-unsync | `StopSyncing` (SAFScript) | ❌ | SHIM-GAP — OSF can't pull one graph out of a live sync group (stop the graph instead) |
| Selection buffer | `AddActorToSelectionBuffer`, `GetSelectionBuffer`, `SelectActor`, `ClearSelectionBuffer`, `GetSelectionBufferSize` | ❌ | SHIM-GAP — OSF exposes the crosshair *target* but not SAF's multi-actor selection buffer |
| Phase/sequence callbacks | `RegisterForPhaseBegin`, `RegisterForSequenceEnd`, `Unregister*` | ❌ | SHIM-GAP — these no-op today (callbacks don't fire). **Fast-follow:** OSF has a richer callback system (`RegisterSceneCallback` + `EVENT_NODE_ENTER`/`EVENT_SCENE_END`) these can be wired to. |

## Known gaps

The SHIM-GAP items above are the only behaviors that don't carry over. In practice they affect a
small slice of SAF content:

- **Absolute repositioning / transform matching** — rare; OSF's scene anchoring covers the common
  "put these actors together" case.
- **Blend-graph variables** — a NAF/SAF blend-graph concept with no OSF analogue.
- **The selection buffer** — superseded by OSF's crosshair-target natives for the common picker case.
- **Phase/sequence-end callbacks** — currently silent; planned to bridge onto OSF's scene-event
  callbacks as a fast-follow (so SAF mods that drove logic off phase/sequence boundaries can be
  pointed at `EVENT_*`).

A SAF mod that uses *only* the ✅/🟡/⚪ rows runs unchanged. A mod relying on a ❌ row will load and
run, but those specific calls are inert (and logged as `SHIM-GAP` in `OSF Animation.log`).

## After migrating — the native path

The shim is for compatibility. New content (and ports of SAF content) should target the native
`OSF.*` scene API directly — graphs of nodes with cues, actions, and callbacks. See
[docs/API.md](API.md) and [SCENE_DESIGN.md](SCENE_DESIGN.md).

## Verifying

Watch `Documents\My Games\Starfield\SFSE\Logs\OSF Animation.log`:
- `[SAF->OSF]` / `[SAFScript->OSF]` lines trace every shimmed call.
- A `SHIM-GAP` line means a call hit an unbridged feature (inert).
- The startup co-load warning means SAF/NAFSF is still installed alongside OSF — disable it.
