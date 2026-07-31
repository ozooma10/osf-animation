#pragma once

#include <cstdint>
#include <string>

namespace RE
{
    class NiAVObject;
    class NiNode;
    class TESBoundObject;

    template <class T>
    class NiPointer;
}

// Verified 1.16.244 scene-graph attach/detach/reparent wrappers (RE'd
// 2026-07-30, Investigations request "Suit Protocol helmet proxy crash").
//
// Engine contract (all runtime-anchored via nonzero AddrLib IDs; the CLSF
// NiNode.h method declarations are WRONG — see the module note
// engine.scene_graph_attach):
//
//   NiAVObject vtable = 84 slots (0x000..0x298). Slot 0 = vector-deleting
//   dtor, slot 1 = DeleteThis (called when refcount 1->0), slot 2 = GetRTTI
//   (returns NiRTTI*: {+0 char* name, +8 NiRTTI* base}). The CLSF base chain
//   (NiRefObject..NiAVObject) matches this; the bug was NiNode.h's added
//   method list: its AddChild/RemoveChild compiled to slots 84/85 = engine
//   AttachChild(child, firstAvail)/SetAt(index, child) — the Suit Protocol
//   crash was RemoveChild(child) dispatching to SetAt with the child pointer
//   truncated into the index and r8 uncontrolled.
//
//   NiNode adds slots 84..94:
//     84 vt+0x2A0  AttachChild(NiAVObject* child, bool firstAvail)    ID 147173
//     85 vt+0x2A8  SetAt(u32 index, NiAVObject* child)                ID 147174
//     86 vt+0x2B0  DetachChild(NiAVObject* child, NiAVObject*& out)   ID 147178
//     87 vt+0x2B8  DetachChild(NiAVObject* child) [no out]            ID 147177
//     88 vt+0x2C0  DetachChildAlt(NiAVObject* child)                  ID 147179
//     89 vt+0x2C8  DetachChildAt(u32 index, NiAVObject*& out)         ID 147176
//     90 vt+0x2D0  DetachChildAt(u32 index) [no out]                  ID 147175
//   NiNode vtable = REL::ID(497979) = 0x4EEFB60 (95 slots).
//
//   Layout (runtime-proven for the fields we touch):
//     NiObject   +0x08 u32 refcount (atomic; release: lock xadd -1, old==1
//                       -> call vtbl[1] DeleteThis)
//     NiAVObject +0x0C u16 childIndex (0xFFFF = detached)
//                +0x0E u16 firstAvail scan hint (nodes only)
//                +0x38 NiNode* parent
//                +0x118 u64 flags
//     NiNode     +0x130 NiTObjectArray vtbl (REL::ID 497983)
//                +0x138 NiAVObject** m_pBase
//                +0x140 u16 maxSize  +0x142 u16 size (used range)
//                +0x144 u16 freeCount +0x146 u16 growBy
//
//   Ownership: AttachChild increfs the child (the parent's array holds one
//   ref). DetachChild(out&) transfers that ref to the caller; the no-out
//   variant releases it (may destroy the child). AttachChild internally calls
//   NiAVObject::SetParent (ID 147223) which auto-detaches from any previous
//   parent via oldParent->vtbl[+0x2B8].
//   DetachChild clears child->parent and sets childIndex = 0xFFFF
//   (via helper ID 147225) before the slot is nulled.
//
//   Threading: the ops take no locks (only the refcounts are atomic). They
//   must run on the game's main thread while the scene-graph update /
//   render-visibility jobs are not walking the tree — i.e. exactly where the
//   engine's own equip pipeline mutates node children. A queued
//   per-frame main-thread service (SFSE task queue / CommandFile pump)
//   satisfies this.
//
// Every wrapper validates the version contract once (REL::ID-resolved vtable
// slots must match the AddrLib function IDs above) and refuses to operate on
// a mismatched runtime. All argument graphs are checked with guarded reads
// before any engine call.
namespace OSF::RE
{
    // Idempotent; resolves + verifies the vtable contract. Returns false (and
    // formats a_err) on an unsupported runtime. Called implicitly by the ops.
    [[nodiscard]] bool EnsureResolved(std::string* a_err = nullptr);

    // One-line resolution summary for diagnostics.
    [[nodiscard]] std::string StatusText();

    // Guarded raw-layout parent readback; safe on any thread.
    [[nodiscard]] ::RE::NiNode* GetParent(::RE::NiAVObject* a_obj);
    // Attach a parentless child to a node. Rejects null/self/cycles, a child
    // that already has a parent (use Reparent), and duplicate entries.
    // Verifies child->parent == parent afterwards. Main thread only.
    [[nodiscard]] bool AttachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child,
        std::string* a_err = nullptr);

    // Detach a child from its (verified) current parent. Holds a strong ref
    // across the engine call so the child survives even if the parent's array
    // held the last reference. Verifies child->parent == null afterwards.
    // Main thread only.
    [[nodiscard]] bool DetachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child,
        std::string* a_err = nullptr);

    // Move a child to a new parent (detach if currently parented + attach),
    // holding a strong ref across the whole move. Main thread only.
    [[nodiscard]] bool Reparent(::RE::NiAVObject* a_child, ::RE::NiNode* a_newParent,
        std::string* a_err = nullptr);

    // Clone the item's WORLD-MODEL visual for a_form (any TESBoundObject)
    // via the engine's real TESBoundObject::Clone3D (vtable slot 0x78 =
    // vt+0x3C0, thunk ID 59627 -> impl slot 0x66 ID 61025 -> core ID 61026;
    // runtime-proven 2026-07-30: populates the out NiPointer with a FRESH,
    // unparented, rc=1 BSFadeNode unique per call, and does NOT mutate the
    // ARMO base form — the earlier contrary static claim was a hex/decimal
    // slot confusion and is retracted). IMPORTANT requester semantics: the
    // engine is called with a NULL requester, which yields the item's ground
    // model ('<Model>_GO' root + BSGeometry children) — passing an actor
    // instead routes ARMO clones into a bare-skeleton rig path with NO
    // geometry (the original Suit Protocol mistake). Verifies the vtable slot
    // fingerprint, the result's unparented/rc=1/has-children state, and that
    // the form's +0x260..0x298 region is unchanged. On success a_out owns one
    // strong ref. Main thread only.
    [[nodiscard]] bool CreateWorldModelVisual(::RE::TESBoundObject* a_form,
        ::RE::NiPointer<::RE::NiAVObject>& a_out,
        std::string* a_err = nullptr);
}
