#include "pch.h"

#include "Util/SceneGraph.h"

#include "RE/N/NiSmartPointer.h"
#include "RE/N/NiNode.h"
#include "RE/T/TESBoundObject.h"

namespace OSF::RE
{
    namespace
    {
        bool Fail(std::string* a_err, const char* a_message)
        {
            if (a_err) {
                *a_err = a_message;
            }
            return false;
        }

        bool WouldCreateCycle(const ::RE::NiNode* a_parent, const ::RE::NiAVObject* a_child)
        {
            for (const auto* node = a_parent; node; node = node->parent) {
                if (node == a_child) {
                    return true;
                }
            }
            return false;
        }
    }

    bool AttachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child, std::string* a_err)
    {
        if (!a_parent || !a_child) {
            return Fail(a_err, "null parent/child");
        }
        if (a_child->parent) {
            return Fail(a_err, "child already has a parent (use Reparent)");
        }
        if (WouldCreateCycle(a_parent, a_child)) {
            return Fail(a_err, "attach would create a cycle");
        }

        a_parent->AttachChild(a_child, true);
        return a_child->parent == a_parent || Fail(a_err, "AttachChild did not attach the child");
    }

    bool DetachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child, std::string* a_err)
    {
        if (!a_parent || !a_child) {
            return Fail(a_err, "null parent/child");
        }
        if (a_child->parent != a_parent) {
            return Fail(a_err, "child does not belong to this parent");
        }

        // The out pointer receives the parent's reference and keeps the child alive
        // until this function returns, even if the parent was its only owner.
        ::RE::NiPointer<::RE::NiAVObject> transferred;
        a_parent->DetachChild(a_child, transferred);
        return !a_child->parent || Fail(a_err, "DetachChild did not detach the child");
    }

    bool Reparent(::RE::NiAVObject* a_child, ::RE::NiNode* a_newParent, std::string* a_err)
    {
        if (!a_child || !a_newParent) {
            return Fail(a_err, "null child/newParent");
        }
        if (a_child->parent == a_newParent) {
            return Fail(a_err, "child already attached to newParent");
        }
        if (WouldCreateCycle(a_newParent, a_child)) {
            return Fail(a_err, "reparent would create a cycle");
        }

        // AttachChild detaches an existing parent. Retain the child across that move.
        const ::RE::NiPointer<::RE::NiAVObject> keepAlive{ a_child };
        a_newParent->AttachChild(a_child, true);
        return a_child->parent == a_newParent || Fail(a_err, "AttachChild did not reparent the child");
    }

    bool CreateWorldModelVisual(::RE::TESBoundObject* a_form,
        ::RE::NiPointer<::RE::NiAVObject>& a_out, std::string* a_err)
    {
        a_out.reset();
        if (!a_form) {
            return Fail(a_err, "null form");
        }

        // A null requester selects the world model; an actor selects the armor rig.
        a_form->Clone3D(nullptr, a_out);
        if (!a_out) {
            return Fail(a_err, "Clone3D produced no visual (model not loadable for this form?)");
        }
        if (a_out->parent) {
            a_out.reset();
            return Fail(a_err, "Clone3D result is already parented");
        }
        return true;
    }
}
