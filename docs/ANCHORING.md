# Anchoring & root-translation modes

How an animation's own **root-bone translation** meets the **world anchor** is the single
convention that decides whether third-party (SAF/NAF-authored) GLBs line up in OSF. OSF did not
author most of the clips it will play, so this cannot be a hidden default — it is a documented,
per-graph, **overridable** contract, set via the `aiRootMode` argument on the anchor primitive
(`OSF.SetAnchor`). This is the one alignment knob expected to need in-game tuning.

## The three modes

| Mode | Value | Root translation | Where the skeleton renders | Use for |
|------|-------|------------------|----------------------------|---------|
| `pin` | `0` (default *with* anchor) | **ignored** | locked at the anchor point every frame | stationary scenes — paired poses, sex, sit/lie idles; GLBs authored at origin |
| `additive` | `1` | **applied**, from the anchor | travels outward as root motion accumulates | locomotion / travelling dances / walk cycles |
| `follow` | `2` (default *without* anchor) | **ignored** | rides the actor's LIVE world transform | in-place anims on an engine-moved actor (the bare `Play` default) |

## Anchor × mode matrix

- **No anchor** (bare `Play`, or after `ClearAnchor`): always behaves as `follow`. The pose plays
  at the actor's feet; the engine is free to walk / shove / ragdoll the actor and the animation
  rides along.
- **Anchor set** (`SetAnchor`):
  - `pin` — the rendered root is forced to the anchor each frame. The physics capsule may settle
    ~0.3 m off; that is **expected** — judge alignment visually, not by `GetWorldTranslation`.
    This is today's compose-root pinning behaviour.
  - `additive` — the anchor is the START point; the clip's root motion moves the skeleton from
    there.
  - `follow` with an anchor set is contradictory; the anchor is ignored and the graph follows the
    actor.

## Why this is the cross-content risk

A SAF GLB may bake root translation that a NAF GLB does not, or vice versa. If OSF picks one fixed
convention, half the ecosystem's content drifts off its mark. Exposing the mode per graph lets a
consumer — and, later, a pack manifest per clip — declare the correct convention instead of
fighting an invisible default. Cross-content alignment lives or dies here.

## Implementation note

Root handling happens in the stamp hook (BGSModelNode::Update vfunc-2 PRE-orig), writing into the
engine flat rig buffers in **NiTransform ROW layout — do NOT transpose** (docs/RE.md). `pin`
overwrites the compose-root translation with the anchor; `additive` composes anchor × root;
`follow` leaves the engine's own root translation in place. No game RE / AddressLib work — this is
internal pose math on bindings OSF already owns.
