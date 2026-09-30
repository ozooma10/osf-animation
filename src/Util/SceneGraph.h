#pragma once

#include <string>

namespace RE
{
    class NiAVObject;
    class NiNode;
    class TESBoundObject;

    template <class T>
    class NiPointer;
}

// Attachment policy over CommonLib's scene-graph operations. Callers own object
// lifetimes and must run on the game thread outside scene-graph traversal.
namespace OSF::RE
{
    // Attach a parentless child. Rejects nulls, existing parents, and cycles.
    [[nodiscard]] bool AttachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child,
        std::string* a_err = nullptr);

    // Detach from the current parent. Releases the parent's reference on return;
    // callers that need the child afterwards must hold their own NiPointer.
    [[nodiscard]] bool DetachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child,
        std::string* a_err = nullptr);

    // Move to a different parent, retaining the child across the move.
    // Rejects nulls, the existing parent, and cycles before changing the tree.
    [[nodiscard]] bool Reparent(::RE::NiAVObject* a_child, ::RE::NiNode* a_newParent,
        std::string* a_err = nullptr);

    // Clone an unparented world-model visual into a_out. The null Clone3D
    // requester selects the ground model rather than the actor's armor rig.
    // On failure a_out is empty; on success it owns the returned visual.
    [[nodiscard]] bool CreateWorldModelVisual(::RE::TESBoundObject* a_form,
        ::RE::NiPointer<::RE::NiAVObject>& a_out,
        std::string* a_err = nullptr);
}
