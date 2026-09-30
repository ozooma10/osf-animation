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

// OSF attachment policy over CommonLibSF's corrected NiNode virtuals and
// NiPointer ownership. The runtime contract is verified once before mutation;
// callers must keep the objects alive and mutate outside scene-graph traversal.
// Checks reject cycles, duplicate attachment, and unexpected ownership changes.
// Child-index and Clone3D integrity checks retain the verified 1.16.244 offsets
// that CommonLib does not yet expose.
namespace OSF::RE
{
    // Idempotent; resolves + verifies the vtable contract. Returns false (and
    // formats a_err) on an unsupported runtime. Called implicitly by the ops.
    [[nodiscard]] bool EnsureResolved(std::string* a_err = nullptr);

    // One-line resolution summary for diagnostics.
    [[nodiscard]] std::string StatusText();

    // Parent readback with a readable-object check; caller owns lifetime and synchronization.
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
