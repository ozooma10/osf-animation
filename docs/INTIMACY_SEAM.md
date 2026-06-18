# Scene-runtime seam — the Layer A ↔ Layer B contract

> **No longer a plugin boundary.** The scene engine **merged back into OSF Animation** as an
> internal module (one DLL, one mod) — see [SCENE_DESIGN.md](SCENE_DESIGN.md) §2.1. This document's
> *contract* still holds and is still load-bearing: the **two-registry model**, the **role→slot
> assignment rule**, the **frozen Tier-0 ABI**, and the **additive scene primitives** all carry
> forward — they now describe an **internal module seam** (Layer A playback core ↔ Layer B scene
> runtime) instead of a cross-plugin seam. The invariant that only the playback core touches the
> fragile animation hooks is unchanged.

*Originally forward-looking, settled 2026-06-14, reconciled after the scene-runtime merge. Records
the boundary between the playback core and the scene runtime so the internal seam stays stable while
OSF ships a frozen 1.0 ABI.*

## Internal layers

- **Layer A playback core** — the **animation registry** + the **playback mechanism** (play / sync /
  anchor / staged advance, the two engine hooks). Content-neutral. The only layer that touches the
  fragile animation playback hooks.
- **Scene runtime (Layer B, in this repo)** — a **scene registry** + **role assignment** + **scene
  composition/navigation**. It builds on Layer A through the public playback surface; it never hooks
  the animation engine directly.
- **Layer C mechanisms (in this repo)** — content-neutral services for equipment, weapon, fade,
  sound/voice, camera, and player control. Layer B drives these through data-authored `osf.*`
  actions and track lanes; the services prologue-gate their game-facing bindings and know nothing
  about scene graphs.

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
multi-stage scene. The scene runtime references animation ids as scene-graph nodes and sequences
them itself.

## Role assignment

The playback core knows **positional slots + gender** only. The scene runtime owns richer role
metadata, filters, complete role binding, and role → slot mapping, then calls Layer A with the
ordered actor array. Layer A never knows what a slot *means*.

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

## Planned additive primitives

These are **purely additive** — natives are never removed or re-signatured within a major
version (API.md), so they slot in without moving the ABI. Add them only when the merged scene
runtime or a real consumer needs them (the core stays lean). Names below are illustrative.

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
independently (API.md). Consumers can pin to an OSF major and the Layer A ↔ B seam won't move under
them.
