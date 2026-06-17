# Scene-runtime seam — the Layer A ↔ Layer B contract (formerly the "OSF Intimacy" seam)

> **No longer a plugin boundary.** The scene engine **merged back into OSF Animation** as an
> internal module (one DLL, one mod) — see [SCENE_DESIGN.md](SCENE_DESIGN.md) §2.1. This document's
> *contract* still holds and is still load-bearing: the **two-registry model**, the **role→slot
> assignment rule**, the **frozen Tier-0 ABI**, and the **additive scene primitives** all carry
> forward — they now describe an **internal module seam** (Layer A playback core ↔ Layer B scene
> runtime) instead of a cross-plugin seam. Read "OSF Intimacy plugin" below as "the Layer-B scene
> runtime subsystem." The invariant that only the playback core touches the engine is unchanged.

*Originally forward-looking, settled 2026-06-14. Records the boundary between the playback core
(OSF Animation) and the scene engine so the seam stays stable while the core ships a frozen 1.0 ABI.*

## The split

- **OSF Animation (this repo, core)** — the **animation registry** + the **playback
  mechanism** (play / sync / anchor / staged advance, the two engine hooks). Content-neutral.
  The ONLY plugin that patches the engine.
- **Scene runtime (Layer B, in this repo)** — a **scene registry** + **role assignment** + **scene
  policy** (undress, scheduled voice, camera/control, fade) + **composition/navigation**.
  Builds *on* the core: does undress/voice/fade itself via CLSF, uses the core's `OSFCompat`
  locks for player control/camera. It never hooks the engine — that stays the core's job, so
  the game-update-fragile surface lives in one place.

## Two registries (the chosen model)

The scene layer is a **separate registry that references core animation ids** (the OStim
model), not a second reader of the same pack file.

| | Animation registry (Layer A) | Scene registry (Layer B, in this repo) |
|---|---|---|
| File | `Data/OSF/**/*.json` (pack) | `Data/OSF/**/*.scene.json` (read by `SceneRegistry`; PackRegistry skips it) |
| Unit | one synced multi-actor **animation** (clips + offsets + gender slots + tags) | a **scene**: references anim ids + policy + sequencing/transitions + roles |
| Reuse | a reusable building block | many scenes reuse one animation |

Scene **policy** (undress / voice / intensity / cues / …) belongs in the **scene file**, not
the anim pack. The core ignores unknown fields, so a pack that still carries them loads fine —
but the recommended home is the scene file.

## Granularity

A core **animation** = one synced multi-actor unit; a **scene** composes/sequences animations.
The pack schema allows `1..N` stages, so a core entry can be a single node OR a simple linear
multi-stage scene — OSF Intimacy references single-stage entries as scene-graph nodes and
sequences them itself.

## Role assignment

The core knows **positional slots + gender** only: `StartScene` fills definition slot `i` with
`actors[i]`; `StartSceneByTags` returns the gender-fit ordering. Richer roles
(dominant/submissive, giver/receiver, …) are OSF Intimacy's — it tracks roles, maps role → slot,
and passes the ordered actor array down. The core never knows what a slot *means*.

## What the core provides today (stable within the 1.0 major ABI)

| Need | Native |
|---|---|
| Play a referenced animation by id | `StartScene(actors, id, stage)` |
| Matchmake + assign by gender slots | `StartSceneByTags(actors, tags)` → chosen id |
| Ad-hoc scene from raw files | `StartSceneFiles(actors, files, speed, blend)` |
| Discover candidates | `FindScenes(actorCount, tags)` |
| Drive stages within a node | `SetSceneStage` / `GetSceneStage` |
| Solo multi-phase | `PlaySequence` |
| Freeze the player (lifecycle owned by the caller) | `OSFCompat.SetPlayer{Control,Camera}Lock` |
| State | `IsPlaying`, `GetCurrentAnimation` |
| Readiness gate | `IsReady`, `HasFeature`, `GetVersion` |
| Pack reload | `ReloadPacks` |

## Planned additive primitives (build *with* OSF Intimacy, not before)

These are **purely additive** — natives are never removed or re-signatured within a major
version (API.md), so they slot in without moving the ABI. Don't add them until Intimacy has a
real use for them (the core stays lean). Names below are illustrative.

1. **`DescribeAnimation(id)` → { actorCount, genderSlots, tags, stageCount }** — lets the scene
   layer validate references and assign roles without re-parsing packs. (Today the scene layer
   would have to load the packs itself.)
2. **`RetargetLiveScene(actor, id | files)`** — swap the playing clips on a LIVE scene while
   keeping the anchor, actors, and locks, for seamless node → node transitions. (Today the path
   is `StopScene` + `StartScene`, which re-anchors and restarts with a fade reset; the player
   lock survives because it's standalone, but the animation visibly resets.)
3. **C ABI over SFSE messaging** ([SCENE_DESIGN.md](SCENE_DESIGN.md) §2.5 Phase D, deferred) —
   efficient C++ → C++ driving so a consumer doesn't round-trip through Papyrus. Gated by the
   readiness handshake.

## ABI promise

Within a major version, natives are never removed or re-signatured and the pack schema versions
independently (API.md). OSF Intimacy can pin to a core major and the seam won't move under it.
