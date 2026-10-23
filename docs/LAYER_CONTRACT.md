# Layer contract (2.0)

Status: design, not started. Written 2026-10-22. Scope decision and rationale live in
[V2_SCOPE.md](V2_SCOPE.md) §2; this document is the contract that section requires.

2.0 specifies an ordered per-actor **layer stack** and implements depth 1. Depth 1 is
behaviourally today's single slot, so nothing in the shipped runtime changes; what changes is that
the general rule is written down and asserted, which is what lets the multi-layer implementation
land later as an additive 2.x instead of a second break.

The contract is stated against the current code so each point is checkable:
`src/Animation/Graph.cpp` (`Sample`, `StampPose`), `src/Animation/PoseMath.h`
(`WriteAdditive`, `WriteOverrideTransition`, `WriteOverrideBlended`, `EffectiveWeight`),
`src/Animation/BoneMask.h`, `src/Animation/FrameClock.h`, `src/Animation/LiveBasePose.h`.

---

## L0. One write point per frame; the stack composes off-buffer

This is the prerequisite for every other point, and the reason a retrofit would be ugly.

Today the rig slot array (`rig->local->data`) is simultaneously **the engine-base source** and
**the output**. `StampPose` captures the engine's evaluated pose out of those slots once per
`enginePoseRevision`, guarded by `stampProbe`: if the probe slot still holds the bytes OSF last
wrote, the engine has not re-evaluated, so the capture is skipped rather than poisoning the base
with our own output.

Layer *N* must read what layer *N-1* wrote this frame. Through the rig buffer that is
indistinguishable from the case the probe exists to reject — our own write from last frame — so a
layered stack that round-trips through the rig buffer would have to weaken the one guard that
keeps the base honest.

**Contract.** The stack composes into OSF's own per-joint buffer and the rig slots are written
once, at the end of the frame's compose:

1. capture `base(0)` from the rig slots exactly as today (probe-gated, once per
   `enginePoseRevision`);
2. compose layers bottom-up in OSF memory;
3. write each driven joint to its rig slot once;
4. record the probe from that final write.

`displayedPose` and `displayedDriven` are already this buffer, with a per-joint driven flag, so
the structure exists. Engine-base capture semantics, `basePoseRevision`, `enginePoseRevision`,
`LiveBasePose::Evaluation` and the probe keep their current meaning and now guard **only** the
engine base. The probe stops being load-bearing for composite correctness, which is a net
robustness gain even at depth 1.

Allocation rule, inherited from today's discipline (`captureLiveBase` refuses to allocate and
returns false instead): **no layer buffer is ever allocated from the stamp hook.** Buffers size at
bind and stage switch, for the declared maximum depth.

---

## L1. What a layer's base is

- `base(0)` is the engine live base: the actor's evaluated pose for this frame.
- `base(N)` is the composite of layers `0..N-1` for this frame, per joint.
- For a joint no lower layer drove, `base(N)` is the engine live base for that joint.

Both write helpers already take the base as a parameter (`a_live` in `WriteAdditive` and
`WriteOverrideTransition`). **The pose math does not change** — only which buffer that pointer
addresses. That is the whole mechanical content of layering, and it is why the math can be
specified now and implemented later without risk of the spec not fitting.

Two consequences to state explicitly, because they are observable:

- `WriteAdditive` preserves the base's scale and pad bytes (`a_slot[15] = liveScale`, pads copied
  from `a_live`). At depth *N* that means the **composite below's** scale propagates, so stacked
  additive layers inherit the bottom layer's scale. Additive scale remains intentionally ignored.
- `WriteOverrideTransition` composes the incoming sample over the base by `strength`, then
  crossfades from the outgoing display by `transition`. At depth *N* the "over the base" half is
  against `base(N)`.

---

## L2. What a mask releases to

**A mask at depth *N* releases its unlisted bones to `base(N)` — the composite below — never to
the engine pose.**

This is the one point that must be stated or depth 1 becomes a permanent special case. At depth 0
`base(0)` *is* the engine pose, so today's documented behaviour ("every other bone stays fully
engine-driven") is the depth-0 instance of the general rule rather than a different rule.

Mechanically: **pre-seed the layer's output with `base(N)` before writing it.** Both helpers have
paths that decline to write — `WriteOverrideTransition` returns false at zero weight or with no
base, and a zero-strength bone is meant to release smoothly — and on a pre-seeded buffer
"declined" correctly means "inherit the composite below" with no extra branch.

Fractional mask weights keep their current meaning, per bone, against `base(N)`:
weight 1 takes the sample absolutely, fractional blends sample-against-base, unlisted inherits.
The feathered-seam masks in `BoneMask.h` therefore feather into the layer below, which is what
makes a torso gesture over an authored position look continuous rather than detached.

---

## L3. How weights compose

Per layer, unchanged in form from today:

- `strength(N, bone) = poseWeight(N) * maskWeight(N, bone)`
- additive uses `EffectiveWeight(transition(N), poseWeight(N)) * maskWeight(N, bone)`
- override passes `transition(N)` and `strength(N, bone)` separately

Rules:

- **There is no stack-wide weight.** A layer's contribution is its own strength against its own
  base. Nothing multiplies across depths; a stack-wide scalar would make a layer's appearance
  depend on how many layers sit under it.
- **Clamp per layer, not once at the end.** Both helpers clamp their inputs to `[0,1]` already;
  that stays per layer so an out-of-range declaration can never reach into another depth.
- **The mask is not applied to the crossfade.** This is already called out in
  `WriteOverrideTransition`'s comment: applying the mask to the transition double-weights
  feathered bones and prevents the incoming layer ever reaching full strength before its snapshot
  disappears. The rule holds per layer.
- Transition weight is **per layer**: each layer owns a `BlendPhase`, `blendClock` and
  `blendDuration`. A gesture fading in does not re-blend the position under it.

---

## L4. Clock ownership

Each layer declares one clock source. The existing types cover all three.

| Source | Behaviour | Use |
|---|---|---|
| `inherit` (default) | the playback session's or solo `SyncGroup`'s clock, exactly as today | layer 0; any layer meant to stay phase-locked to the position |
| `own` | an independent `FrameClock` owned by the layer | a one-shot gesture whose length is unrelated to the position's loop |
| `group:<id>` | a named `SyncGroup` shared across actors | a paired gesture that must stay in step on both participants |

Points that have to be in the contract:

- `SyncGroup` is deliberately **not wrapped** — "each graph wraps by its own clip duration, so
  mismatched lengths share a phase origin". That property is what makes `inherit` usable for a
  layer whose clip length differs from the position's, and it is preserved.
- An `own`-clock layer needs its own `holdClipAtEnd`, so a one-shot clamps at its last frame
  (`localTime = min(clock.time, duration)`) instead of wrapping.
- **Scene speed applies to every clock by default, including `own`.** A layer may not opt out
  silently. Pause is speed zero in practice, and a gesture that kept running through the panel's
  pause would be a visible defect rather than a nuance.
- Owner-token gating (`FrameClock::ShouldAdvance`, `kClockOwnerStaleMs`) applies per clock
  unchanged: `Sample` runs several times per frame per graph and only the owning manager advances
  time. Adding clocks does not add clock reads — `Sample` already takes one timestamp per call and
  shares it.

---

## L5. Lifetime, removal, and what a blend-out lands on

- **A layer blending out lands on `base(N)`**, the composite below it — the layering generalisation
  of today's "blend-out keeps sampling while weight ramps to 0 and lands on the live pose".
- The outgoing snapshot (`blendFromPose`, `blendFromDriven`, `BlendSource::Prepared`) becomes
  per layer. Today's rule "only joints the outgoing stage stamped have a valid snapshot; newly
  masked-in joints blend from live" generalises to *from `base(N)`*.
- A one-shot layer self-removes after its fade completes; a sustained layer persists until removed.
  Removal is queued the way `RemoveFadedGraph` is today rather than taken inside the compose.
- **Removing layer *N* must not disturb layers above it.** They recompose against the new,
  shallower composite on the next frame. This requires that per-joint driven flags are recomputed
  each frame rather than carried across frames — `StampPose` already clears `displayedDriven` at
  the top of every compose, and that must stay true per layer.
- Depth is bounded by a declared maximum (L0's allocation rule). A request that would exceed it
  refuses with a readable reason at request time, never mid-frame.

---

## L6. What is per layer and what stays per role or participant

| Declaration | Scope | Why |
|---|---|---|
| clip, `poseMode`, `poseWeight`, `mask`, blend time, loop/hold, clock source | **per layer** | these are what a layer *is* |
| `preserveBones` | **per role**, and outranks every layer | it means "never write this bone"; a per-layer exclusion would let a higher layer reintroduce a bone the role excluded |
| world placement, anchor, offset, alignment delta | **per participant** | a gesture does not move the actor; placement belongs to the body, not a layer |
| role filters, `equip`, anatomy requirements | **per role** | casting is settled before any layer exists |

Data shape: a playback segment's per-participant arrays (`anims`, `poseModes`, `poseWeights`,
`masks`, `placements`, indexed by participant, applied at the stage switch in `Sample`) become
indexed by **(participant, layer)**, except `placements`, which stays per participant.

---

## L7. Depth-1 conformance, and what the suites assert

The point of specifying now and implementing depth 1 is that depth 1 must be provably the
general rule's one-layer case, not a parallel path. The engine-free pose suites assert:

1. With one layer, `base(0)` is the engine base and a mask releases to the engine base.
2. For identical inputs, the composed result is **byte-identical** to the current
   `WriteAdditive` / `WriteOverrideTransition` output — including the exact no-op fast path
   (weight 0 or a rest-pose sample copies the base verbatim) and the preserved pad/scale bytes.
3. Pre-seeding plus a declined write equals today's "leave the slot untouched" at depth 0.
4. A second layer declared at depth 1 in a fixture is refused by the depth-1 runtime with the
   readable reason, rather than silently ignored.

Assertions 1-3 are the equality invariant; they are what make the later multi-layer change a
refactor with a test oracle instead of a rewrite. The pattern mirrors OSF Intimacy's vocabulary
derivation fixture: the expected values live in the fixture, not duplicated in the thing under
test.

---

### L7 fixture sketch

Not committed as code: the fixture tests a composition seam that does not exist yet, so writing it
as a compiling suite means implementing that seam, which is step A of
[V2_SCOPE.md](V2_SCOPE.md) §8 and is not to start while `osf/paired-animation-placement` is in
flight. The sketch below is the shape to build when it does.

**The seam.** Stack composition goes in a new `src/Animation/LayerCompose.h`, dependency-free in
the same way `PoseMath.h` and `BoneMask.h` are, because the engine-free suites include only that
set. It does **not** reuse `Scene.h`'s `PoseMode`: `Scene.h` carries engine references and ozz
types, so a pure suite cannot include it. The mode is declared locally and mapped at the call
site in `StampPose`.

```cpp
namespace OSF::Animation::LayerCompose
{
    enum class Mode : std::uint8_t { kOverride, kAdditive };

    struct Bone { std::uint16_t jointIndex; float maskWeight; };

    struct Layer
    {
        Mode  mode = Mode::kOverride;
        float poseWeight = 1.0f;
        float transition = 1.0f;              // this layer's blend weight
        const float* sampled = nullptr;        // 16 floats per joint
        const float* reference = nullptr;      // additive only
        const float* displayed = nullptr;      // outgoing snapshot, may be null
        const std::uint8_t* displayedDriven = nullptr;
        const Bone* bones = nullptr;
        std::size_t boneCount = 0;
    };

    // a_out is pre-seeded with base(0) per bound joint (L2); a_drivenOut is cleared.
    // a_haveBase is false when the binding has not been engine-evaluated yet.
    // Returns false without writing if the stack exceeds the implemented depth (L5).
    bool Compose(const Layer* a_layers, std::size_t a_layerCount, bool a_haveBase,
        float* a_out, std::uint8_t* a_drivenOut, std::size_t a_jointCount);
}
```

`Compose` must **delegate** to `WriteAdditive` / `WriteOverrideTransition` rather than restate
their math. The assertions below are written with `std::memcmp`, not `Near`, specifically to hold
it to that: a reimplementation that is numerically close would pass a tolerance comparison and
then diverge silently the first time either helper is touched.

**Registration**, mirroring `osf-additive-pose-test` exactly:

```lua
target("osf-layer-pose-test")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("test/LayerPoseTest.cpp")
    add_includedirs("src")
```

That takes the engine-free suite count from fourteen to fifteen, so the count quoted in
[API.md](API.md) and the release acceptance notes moves with it.

**`test/LayerPoseTest.cpp`**, in the existing idiom — `main()` with scoped blocks,
`Check(cond, "prose label")`, the local `Matrix`/`Transform`/`AxisAngle` helpers from
`AdditivePoseTest.cpp`, `Finish` at the end:

```cpp
// L7.1 - override equality, including a declined write
{
    const Matrix base = Transform(AxisAngle(1, 0, 0, kHalfPi), 1.0f, 2.0f, 3.0f);
    const Matrix sampled = Transform(AxisAngle(0, 0, 1, kHalfPi), 4.0f, 5.0f, 6.0f);
    const Matrix displayed = Transform(AxisAngle(0, 1, 0, kHalfPi), 7.0f, 8.0f, 9.0f);

    struct Case { float transition; float strength; bool snapshot; bool haveBase; };
    for (const Case c : { Case{1.0f, 1.0f, false, true},    // full override
                          Case{1.0f, 0.5f, false, true},    // feathered bone
                          Case{0.4f, 1.0f, true,  true},    // mid-crossfade
                          Case{1.0f, 0.5f, true,  false},   // no proven base, snapshot held
                          Case{0.0f, 1.0f, false, true},    // declined: transition 0
                          Case{1.0f, 0.0f, false, true} }) { // declined: strength 0
        Matrix expected = base;  // pre-seeded, so "declined" means "keep the base"
        WriteOverrideTransition(expected.data(), c.haveBase ? base.data() : nullptr,
            sampled.data(), c.snapshot ? displayed.data() : nullptr, c.transition, c.strength);

        Matrix actual = base;
        std::uint8_t driven = 0;
        // one layer, one bone at joint 0, maskWeight folded into strength by the caller
        const auto layer = MakeLayer(Mode::kOverride, c, sampled, displayed);
        Check(Compose(&layer, 1, c.haveBase, actual.data(), &driven, 1), "depth 1 composes");
        Check(std::memcmp(actual.data(), expected.data(), sizeof(Matrix)) == 0,
            "depth-1 override is byte-identical to WriteOverrideTransition");
    }
}

// L7.2 - additive equality, the no-op fast path, and preserved pad/scale bytes
{
    Matrix base = Transform(AxisAngle(0, 0, 1, kHalfPi), 1.0f, 2.0f, 3.0f, 1.25f);
    base[3] = 0.5f; base[7] = -0.25f; base[11] = 0.75f;  // pads must survive verbatim
    const Matrix reference = Transform();
    const Matrix sampled = Transform(AxisAngle(1, 0, 0, kHalfPi), 0.0f, 1.0f, 0.0f);

    for (const float weight : { 1.0f, 0.5f, 0.0f }) {
        Matrix expected = base;
        WriteAdditive(expected.data(), base.data(), reference.data(), sampled.data(), weight);
        Matrix actual = base;
        std::uint8_t driven = 0;
        const auto layer = MakeAdditive(weight, sampled, reference);
        Check(Compose(&layer, 1, true, actual.data(), &driven, 1), "depth 1 composes additive");
        Check(std::memcmp(actual.data(), expected.data(), sizeof(Matrix)) == 0,
            "depth-1 additive is byte-identical to WriteAdditive");
    }

    // The exact no-op: a rest-pose sample copies the base verbatim, pads and scale included.
    Matrix noop = base;
    std::uint8_t driven = 0;
    const auto rest = MakeAdditive(1.0f, reference, reference);
    Check(Compose(&rest, 1, true, noop.data(), &driven, 1) &&
        std::memcmp(noop.data(), base.data(), sizeof(Matrix)) == 0,
        "a rest-pose additive sample is a byte-for-byte no-op through Compose");
}

// L7.3 - pre-seeding equals today's untouched slot
{
    // WriteOverrideTransition returns false and leaves the slot alone; the engine value stands.
    Matrix slot = Transform(AxisAngle(0, 1, 0, kHalfPi), 3.0f, 2.0f, 1.0f);
    const Matrix untouched = slot;
    Check(!WriteOverrideTransition(slot.data(), untouched.data(), Transform().data(),
              nullptr, 0.0f, 1.0f) &&
        std::memcmp(slot.data(), untouched.data(), sizeof(Matrix)) == 0,
        "a declined override leaves the slot byte-identical");
    // Compose reaches the same bytes by inheritance rather than by not writing.
}

// L7.4 - a second layer is refused, not ignored
{
    const std::array<Layer, 2> stack{ /* two depth-0-shaped layers */ };
    Matrix out = Transform();
    std::uint8_t driven = 0;
    Check(!Compose(stack.data(), stack.size(), true, out.data(), &driven, 1),
        "the depth-1 runtime refuses a two-layer stack");
    Check(driven == 0, "a refused stack writes nothing and drives nothing");
}

// L2 at depth 0 - a mask releases to the engine base
{
    // Two bound joints, one in the mask and one not; the unmasked joint keeps base bytes.
}
```

`MakeLayer` / `MakeAdditive` are fixture-local constructors in the file's anonymous namespace,
following `Transform`'s precedent, so each case reads as data rather than nine field assignments.

What this buys: assertions 1-3 are the equality invariant, and they turn the later multi-layer
change into a refactor with an oracle. If `Compose` ever stops delegating, or either helper's
behaviour shifts, the fixture fails at depth 1 — before any multi-layer code exists to blame.

---

## L8. Ordering authority

Content declares a layer's **slot class**, and the runtime orders the stack deterministically by
slot class, then by request order within a class. Two independent callers adding gesture layers
must not be able to produce a frame-order-dependent pose.

This matches the arbitration rule OSF Intimacy's provider model already uses: one owner per
domain, deterministic arbitration, conflicts logged. The slot-class set itself is content-neutral
and intentionally small; naming it is part of the 2.0 design, not of this contract.
