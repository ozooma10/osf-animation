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
    // Rejects nulls, existing parents, and cycles.
    [[nodiscard]] bool AttachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child,
        std::string* a_err = nullptr);

    // Releases the parent's reference; hold your own NiPointer to keep the child alive.
    [[nodiscard]] bool DetachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child,
        std::string* a_err = nullptr);

    // Rejects nulls, the existing parent, and cycles before changing the tree.
    [[nodiscard]] bool Reparent(::RE::NiAVObject* a_child, ::RE::NiNode* a_newParent,
        std::string* a_err = nullptr);

    // Null Clone3D requester selects the ground model, not the actor's armor rig.
    [[nodiscard]] bool CreateWorldModelVisual(::RE::TESBoundObject* a_form,
        ::RE::NiPointer<::RE::NiAVObject>& a_out,
        std::string* a_err = nullptr);
}
