#include "pch.h"

#include "Util/SceneGraph.h"

#include "Util/StarfieldRuntime.h"

#include "RE/N/NiSmartPointer.h"
#include "RE/T/TESBoundObject.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

// Implementation notes live in SceneGraph.h and the module note
// engine.scene_graph_attach. Everything here works on RAW offsets — the CLSF
// NiNode/NiAVObject member and method declarations are not trusted (their
// vtable indices are provably wrong on 1.16.244 and caused the Suit Protocol
// crash this wrapper replaces).
namespace OSF::RE
{
    namespace
    {
        // --- verified layout (1.16.244) ---
        constexpr std::ptrdiff_t kRefCount = 0x08;
        constexpr std::ptrdiff_t kChildIndex = 0x0C;    // u16; 0xFFFF = detached
        constexpr std::ptrdiff_t kParent = 0x38;
        constexpr std::ptrdiff_t kChildrenBase = 0x138; // NiAVObject** m_pBase
        constexpr std::ptrdiff_t kChildrenMax = 0x140;  // u16 maxSize
        constexpr std::ptrdiff_t kChildrenSize = 0x142; // u16 size (used range)

        // --- vtable slot byte offsets (NiNode-added virtuals) ---
        constexpr std::ptrdiff_t kSlotDeleteThis = 0x08;
        constexpr std::ptrdiff_t kSlotGetRtti = 0x10;
        constexpr std::ptrdiff_t kSlotAttachChild = 0x2A0;
        constexpr std::ptrdiff_t kSlotSetAt = 0x2A8;
        constexpr std::ptrdiff_t kSlotDetachChildOut = 0x2B0;
        constexpr std::ptrdiff_t kSlotDetachChild = 0x2B8;

        // --- AddrLib IDs (all verified nonzero + unique on 1.16.244) ---
        constexpr std::uint64_t kIdNiNodeVtbl = 497979;       // 0x4EEFB60
        constexpr std::uint64_t kIdAttachChild = 147173;      // 0x2BD15E0
        constexpr std::uint64_t kIdSetAt = 147174;            // 0x2BD1820
        constexpr std::uint64_t kIdDetachChildOut = 147178;   // 0x2BD1B20
        constexpr std::uint64_t kIdDetachChild = 147177;      // 0x2BD1AD0

        constexpr std::size_t kNiNodeSize = 0x150;
        constexpr std::uint16_t kDetachedIndex = 0xFFFF;
        constexpr int kMaxAncestorHops = 128;

        using AttachChildFn = void (*)(void* a_node, void* a_child, bool a_firstAvail);
        using DetachChildOutFn = void (*)(void* a_node, void* a_child, void** a_out);
        using DeleteThisFn = void (*)(void* a_obj);
        using GetRttiFn = void* (*)(void* a_obj);

        struct Resolved
        {
            bool ok{ false };
            std::string detail;
            std::uintptr_t niNodeVtbl{ 0 };
            std::uintptr_t attachChild{ 0 };
            std::uintptr_t detachChildOut{ 0 };
            const void* niNodeRtti{ nullptr };  // canonical NiRTTI for the chain walk
            std::uintptr_t textBegin{ 0 };
            std::uintptr_t textEnd{ 0 };
        };

        Resolved g_resolved;
        std::once_flag g_resolveOnce;

        [[nodiscard]] std::uintptr_t Addr(const std::uintptr_t a_obj) noexcept
        {
            return a_obj;
        }

        [[nodiscard]] std::uintptr_t Addr(const void* a_obj) noexcept
        {
            return reinterpret_cast<std::uintptr_t>(a_obj);
        }

        [[nodiscard]] bool ReadU16(const std::uintptr_t a_addr, std::uint16_t& a_out)
        {
            if (!Util::IsReadableRange(a_addr, sizeof(a_out))) {
                return false;
            }
            std::memcpy(&a_out, reinterpret_cast<const void*>(a_addr), sizeof(a_out));
            return true;
        }

        [[nodiscard]] bool ReadU32(const std::uintptr_t a_addr, std::uint32_t& a_out)
        {
            if (!Util::IsReadableRange(a_addr, sizeof(a_out))) {
                return false;
            }
            std::memcpy(&a_out, reinterpret_cast<const void*>(a_addr), sizeof(a_out));
            return true;
        }

        void Fail(std::string* a_err, std::string a_msg)
        {
            if (a_err) {
                *a_err = std::move(a_msg);
            }
        }

        // Starfield.exe image bounds (code pointers we dispatch through must
        // land inside the module).
        void ResolveImageBounds(Resolved& a_r)
        {
            const auto base = Util::GetStarfieldBase();
            if (base == 0) {
                return;
            }
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            a_r.textBegin = base;
            a_r.textEnd = base + nt->OptionalHeader.SizeOfImage;
        }

        [[nodiscard]] bool InImage(const Resolved& a_r, const std::uintptr_t a_addr) noexcept
        {
            return a_addr >= a_r.textBegin && a_addr < a_r.textEnd;
        }

        void ResolveImpl()
        {
            auto& r = g_resolved;
            ResolveImageBounds(r);
            if (r.textBegin == 0) {
                r.detail = "Starfield.exe module not found";
                return;
            }

            const auto resolve = [](const std::uint64_t a_id) -> std::uintptr_t {
                const ::REL::ID id{ static_cast<std::uint64_t>(a_id) };
                return ::REL::Relocation<std::uintptr_t>{ id }.address();
            };

            r.niNodeVtbl = resolve(kIdNiNodeVtbl);
            r.attachChild = resolve(kIdAttachChild);
            r.detachChildOut = resolve(kIdDetachChildOut);
            const auto setAt = resolve(kIdSetAt);
            const auto detachNoOut = resolve(kIdDetachChild);

            if (!r.niNodeVtbl || !r.attachChild || !r.detachChildOut) {
                r.detail = "AddrLib ID resolution failed (wrong/missing versionlib?)";
                return;
            }

            // The version contract: the canonical NiNode vtable's attach/detach
            // slots must contain exactly the functions the IDs name. If the
            // engine layout moved, this mismatches and we refuse to operate.
            struct SlotCheck
            {
                std::ptrdiff_t slot;
                std::uintptr_t expected;
                const char* name;
            };
            const SlotCheck checks[] = {
                { kSlotAttachChild, r.attachChild, "AttachChild" },
                { kSlotSetAt, setAt, "SetAt" },
                { kSlotDetachChildOut, r.detachChildOut, "DetachChild(out)" },
                { kSlotDetachChild, detachNoOut, "DetachChild" },
            };
            for (const auto& c : checks) {
                std::uintptr_t got = 0;
                if (!Util::SafeReadQword(r.niNodeVtbl + c.slot, got) || got != c.expected) {
                    r.detail = std::format(
                        "vtable contract mismatch: NiNode vtbl+0x{:X} ({}) = 0x{:X}, expected 0x{:X}",
                        static_cast<std::size_t>(c.slot), c.name, Util::ToRva(got),
                        Util::ToRva(c.expected));
                    return;
                }
            }

            // Canonical NiNode NiRTTI: slot 2 GetRTTI is `lea rax,[rip+X]; ret`
            // (no this-use); call it once to anchor NiNode checks.
            std::uintptr_t getRtti = 0;
            if (!Util::SafeReadQword(r.niNodeVtbl + kSlotGetRtti, getRtti) ||
                !InImage(r, getRtti)) {
                r.detail = "NiNode GetRTTI slot unreadable";
                return;
            }
            r.niNodeRtti = reinterpret_cast<GetRttiFn>(getRtti)(nullptr);
            if (!r.niNodeRtti) {
                r.detail = "NiNode GetRTTI returned null";
                return;
            }

            r.ok = true;
            r.detail = std::format(
                "ok: NiNode vtbl=0x{:X} AttachChild=0x{:X} DetachChild(out)=0x{:X} rtti=0x{:X}",
                Util::ToRva(r.niNodeVtbl), Util::ToRva(r.attachChild),
                Util::ToRva(r.detachChildOut), Util::ToRva(Addr(r.niNodeRtti)));
        }

        [[nodiscard]] const Resolved& Resolve()
        {
            std::call_once(g_resolveOnce, ResolveImpl);
            return g_resolved;
        }

        // Guarded object sanity: vtable readable and inside the image.
        [[nodiscard]] bool ReadVtbl(const Resolved& a_r, const void* a_obj, std::uintptr_t& a_vtbl)
        {
            return a_obj != nullptr &&
                   Util::SafeReadQword(Addr(a_obj), a_vtbl) &&
                   InImage(a_r, a_vtbl);
        }

        // Engine NiRTTI chain walk: rtti = obj->vtbl[2](obj); while (rtti)
        // rtti = rtti->base(+8). Matches the engine's own ancestor test
        // (0x142BD4510 in helper ID 147225).
        [[nodiscard]] bool RttiChainContains(const Resolved& a_r, const void* a_obj, const void* a_target)
        {
            std::uintptr_t vtbl = 0;
            if (!ReadVtbl(a_r, a_obj, vtbl)) {
                return false;
            }
            std::uintptr_t getRtti = 0;
            if (!Util::SafeReadQword(vtbl + kSlotGetRtti, getRtti) || !InImage(a_r, getRtti)) {
                return false;
            }
            auto rtti = Addr(reinterpret_cast<GetRttiFn>(getRtti)(const_cast<void*>(a_obj)));
            for (int i = 0; i < 32 && rtti != 0; ++i) {
                if (rtti == Addr(a_target)) {
                    return true;
                }
                if (!Util::SafeReadQword(rtti + 8, rtti)) {
                    return false;
                }
            }
            return false;
        }

        // Fetch a dispatchable NiNode virtual from the object's OWN vtable
        // (honors subclass overrides), verifying it lands in the image.
        [[nodiscard]] std::uintptr_t NodeVirtual(const Resolved& a_r, const void* a_node,
            const std::ptrdiff_t a_slot)
        {
            std::uintptr_t vtbl = 0;
            if (!ReadVtbl(a_r, a_node, vtbl)) {
                return 0;
            }
            std::uintptr_t fn = 0;
            if (!Util::SafeReadQword(vtbl + a_slot, fn) || !InImage(a_r, fn)) {
                return 0;
            }
            return fn;
        }

        // Children array snapshot (guarded).
        struct ChildArray
        {
            std::uintptr_t base{ 0 };
            std::uint16_t size{ 0 };
            std::uint16_t maxSize{ 0 };
        };

        [[nodiscard]] bool ReadChildArray(const void* a_node, ChildArray& a_out)
        {
            const auto node = Addr(a_node);
            std::uintptr_t base = 0;
            if (!Util::SafeReadQword(node + kChildrenBase, base)) {
                return false;
            }
            std::uint16_t size = 0;
            std::uint16_t maxSize = 0;
            if (!ReadU16(node + kChildrenSize, size) || !ReadU16(node + kChildrenMax, maxSize)) {
                return false;
            }
            if (size > maxSize ||
                (size != 0 && (base == 0 || !Util::IsReadableRange(base, size * sizeof(void*))))) {
                return false;
            }
            a_out = { base, size, maxSize };
            return true;
        }

        [[nodiscard]] int CountEntries(const void* a_parent, const void* a_child)
        {
            ChildArray arr{};
            if (!ReadChildArray(a_parent, arr)) {
                return -1;
            }
            int hits = 0;
            for (std::uint16_t i = 0; i < arr.size; ++i) {
                std::uintptr_t entry = 0;
                if (Util::SafeReadQword(arr.base + i * sizeof(void*), entry) &&
                    entry == Addr(a_child)) {
                    ++hits;
                }
            }
            return hits;
        }

        // a_node or any of its ancestors == a_candidate? (cycle guard)
        [[nodiscard]] bool IsSelfOrAncestor(const void* a_node, const void* a_candidate)
        {
            auto cursor = Addr(a_node);
            for (int i = 0; i < kMaxAncestorHops && cursor != 0; ++i) {
                if (cursor == Addr(a_candidate)) {
                    return true;
                }
                if (!Util::SafeReadQword(cursor + kParent, cursor)) {
                    return false;
                }
            }
            return false;
        }

        void RawIncRef(const void* a_obj) noexcept
        {
            ::InterlockedIncrement(
                reinterpret_cast<volatile LONG*>(Addr(a_obj) + kRefCount));
        }

        // Engine-exact release: on 1 -> 0 call vtbl[1] DeleteThis.
        void RawDecRef(const Resolved& a_r, const void* a_obj) noexcept
        {
            const auto old = ::InterlockedDecrement(
                                 reinterpret_cast<volatile LONG*>(Addr(a_obj) + kRefCount)) +
                             1;
            if (old == 1) {
                std::uintptr_t vtbl = 0;
                std::uintptr_t deleter = 0;
                if (Util::SafeReadQword(Addr(a_obj), vtbl) &&
                    Util::SafeReadQword(vtbl + kSlotDeleteThis, deleter) &&
                    InImage(a_r, deleter)) {
                    reinterpret_cast<DeleteThisFn>(deleter)(const_cast<void*>(a_obj));
                }
            }
        }

        // Shared pre-flight for the mutating ops.
        [[nodiscard]] bool ValidateNodePair(const Resolved& a_r, ::RE::NiNode* a_parent,
            ::RE::NiAVObject* a_child, std::string* a_err)
        {
            if (!a_parent || !a_child) {
                Fail(a_err, "null parent/child");
                return false;
            }
            if (Addr(a_parent) == Addr(a_child)) {
                Fail(a_err, "parent == child");
                return false;
            }
            if (!Util::IsReadableRange(Addr(a_parent), kNiNodeSize) ||
                !Util::IsReadableRange(Addr(a_child), 0x130)) {
                Fail(a_err, "parent/child memory unreadable");
                return false;
            }
            if (!RttiChainContains(a_r, a_parent, a_r.niNodeRtti)) {
                Fail(a_err, "parent is not a NiNode (engine NiRTTI chain)");
                return false;
            }
            ChildArray arr{};
            if (!ReadChildArray(a_parent, arr)) {
                Fail(a_err, "parent child-array invalid/unreadable");
                return false;
            }
            return true;
        }
    }

    bool EnsureResolved(std::string* a_err)
    {
        const auto& r = Resolve();
        if (!r.ok) {
            Fail(a_err, r.detail);
        }
        return r.ok;
    }

    std::string StatusText()
    {
        const auto& r = Resolve();
        return r.detail;
    }

    ::RE::NiNode* GetParent(::RE::NiAVObject* a_obj)
    {
        std::uintptr_t parent = 0;
        if (!a_obj || !Util::SafeReadQword(Addr(a_obj) + kParent, parent)) {
            return nullptr;
        }
        return reinterpret_cast<::RE::NiNode*>(parent);
    }

    static std::uint16_t GetChildIndex(::RE::NiAVObject* a_obj)
    {
        std::uint16_t idx = kDetachedIndex;
        if (a_obj) {
            (void)ReadU16(Addr(a_obj) + kChildIndex, idx);
        }
        return idx;
    }

    static std::uint32_t GetRefCount(::RE::NiAVObject* a_obj)
    {
        std::uint32_t rc = 0;
        if (a_obj) {
            (void)ReadU32(Addr(a_obj) + kRefCount, rc);
        }
        return rc;
    }

    bool AttachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child, std::string* a_err)
    {
        const auto& r = Resolve();
        if (!r.ok) {
            Fail(a_err, r.detail);
            return false;
        }
        if (!ValidateNodePair(r, a_parent, a_child, a_err)) {
            return false;
        }
        const auto currentParent = GetParent(a_child);
        if (currentParent == a_parent) {
            Fail(a_err, "child already attached to this parent");
            return false;
        }
        if (currentParent != nullptr) {
            Fail(a_err, "child already has a parent (use Reparent)");
            return false;
        }
        if (IsSelfOrAncestor(a_parent, a_child)) {
            Fail(a_err, "attach would create a cycle (child is an ancestor of parent)");
            return false;
        }
        if (CountEntries(a_parent, a_child) != 0) {
            Fail(a_err, "child already present in parent's child array");
            return false;
        }

        const auto fn = NodeVirtual(r, a_parent, kSlotAttachChild);
        if (fn == 0) {
            Fail(a_err, "parent AttachChild slot unreadable/out of image");
            return false;
        }
        reinterpret_cast<AttachChildFn>(fn)(a_parent, a_child, true);

        // Post-conditions.
        if (GetParent(a_child) != a_parent) {
            Fail(a_err, "post: child->parent != parent after AttachChild");
            return false;
        }
        const auto idx = GetChildIndex(a_child);
        const auto hits = CountEntries(a_parent, a_child);
        if (idx == kDetachedIndex || hits != 1) {
            Fail(a_err, std::format("post: childIndex=0x{:X} entries={} after AttachChild", idx, hits));
            return false;
        }
        return true;
    }

    bool DetachChild(::RE::NiNode* a_parent, ::RE::NiAVObject* a_child, std::string* a_err)
    {
        const auto& r = Resolve();
        if (!r.ok) {
            Fail(a_err, r.detail);
            return false;
        }
        if (!ValidateNodePair(r, a_parent, a_child, a_err)) {
            return false;
        }
        if (GetParent(a_child) != a_parent) {
            Fail(a_err, "child->parent != parent (refusing detach)");
            return false;
        }
        const auto idx = GetChildIndex(a_child);
        ChildArray arr{};
        if (idx == kDetachedIndex || !ReadChildArray(a_parent, arr) || idx >= arr.size) {
            Fail(a_err, std::format("childIndex 0x{:X} invalid for parent's array", idx));
            return false;
        }
        std::uintptr_t slotValue = 0;
        if (!Util::SafeReadQword(arr.base + idx * sizeof(void*), slotValue) ||
            slotValue != Addr(a_child)) {
            Fail(a_err, "parent's array slot does not hold the child");
            return false;
        }

        const auto fn = NodeVirtual(r, a_parent, kSlotDetachChildOut);
        if (fn == 0) {
            Fail(a_err, "parent DetachChild slot unreadable/out of image");
            return false;
        }

        // Strong guard so the child cannot be destroyed mid-operation; the
        // out-pointer variant transfers the array's reference to us and we
        // release it after verification.
        RawIncRef(a_child);
        void* transferred = nullptr;
        reinterpret_cast<DetachChildOutFn>(fn)(a_parent, a_child, &transferred);

        bool ok = true;
        if (GetParent(a_child) != nullptr) {
            Fail(a_err, "post: child->parent still set after DetachChild");
            ok = false;
        } else if (GetChildIndex(a_child) != kDetachedIndex) {
            Fail(a_err, "post: childIndex not 0xFFFF after DetachChild");
            ok = false;
        } else if (CountEntries(a_parent, a_child) != 0) {
            Fail(a_err, "post: child still present in parent's array");
            ok = false;
        } else if (transferred != a_child) {
            Fail(a_err, "post: DetachChild out-pointer did not return the child");
            ok = false;
        }

        if (transferred) {
            RawDecRef(r, transferred);  // release the array's transferred ref
        }
        RawDecRef(r, a_child);  // release our guard
        return ok;
    }

    bool Reparent(::RE::NiAVObject* a_child, ::RE::NiNode* a_newParent, std::string* a_err)
    {
        const auto& r = Resolve();
        if (!r.ok) {
            Fail(a_err, r.detail);
            return false;
        }
        if (!a_child || !a_newParent) {
            Fail(a_err, "null child/newParent");
            return false;
        }
        if (IsSelfOrAncestor(a_newParent, a_child)) {
            Fail(a_err, "reparent would create a cycle");
            return false;
        }

        RawIncRef(a_child);  // survive the detach window
        bool ok = true;
        const auto oldParent = GetParent(a_child);
        if (oldParent == a_newParent) {
            Fail(a_err, "child already attached to newParent");
            ok = false;
        }
        if (ok && oldParent != nullptr) {
            ok = DetachChild(reinterpret_cast<::RE::NiNode*>(oldParent), a_child, a_err);
        }
        if (ok) {
            ok = AttachChild(a_newParent, a_child, a_err);
        }
        RawDecRef(r, a_child);
        return ok;
    }

    bool CreateWorldModelVisual(::RE::TESBoundObject* a_form,
        ::RE::NiPointer<::RE::NiAVObject>& a_out,
        std::string* a_err)
    {
        constexpr std::uint64_t kIdClone3DThunk = 59627;  // vt slot 0x78 on 1.16.244
        constexpr std::ptrdiff_t kClone3DSlot = 0x78 * 8;

        const auto& r = Resolve();
        if (!r.ok) {
            Fail(a_err, r.detail);
            return false;
        }
        if (!a_form) {
            Fail(a_err, "null form");
            return false;
        }
        a_out.reset();

        // Version fingerprint: the object's Clone3D slot must be the proven thunk.
        std::uintptr_t vt = 0, slotFn = 0;
        if (!Util::SafeReadQword(Addr(a_form), vt) || !InImage(r, vt) ||
            !Util::SafeReadQword(vt + kClone3DSlot, slotFn)) {
            Fail(a_err, "form vtable unreadable");
            return false;
        }
        const auto expected =
            ::REL::Relocation<std::uintptr_t>{ ::REL::ID(kIdClone3DThunk) }.address();
        if (slotFn != expected) {
            Fail(a_err, std::format("Clone3D slot mismatch (0x{:X} != expected 0x{:X}) — unsupported runtime",
                Util::ToRva(slotFn), Util::ToRva(expected)));
            return false;
        }

        // Base-form integrity snapshot (+0x260..0x298 — runtime-proven invariant).
        std::array<std::uintptr_t, 8> before{};
        for (std::size_t i = 0; i < before.size(); ++i) {
            (void)Util::SafeReadQword(Addr(a_form) + 0x260 + i * 8, before[i]);
        }

        // A null requester selects the item's world model; an actor would route
        // armor clones into a bare-skeleton path with no geometry.
        a_form->Clone3D(nullptr, a_out);

        if (!a_out) {
            Fail(a_err, "Clone3D produced no visual (model not loadable for this form?)");
            return false;
        }
        {
            std::uint16_t kids = 0;
            if (!ReadU16(Addr(a_out.get()) + kChildrenSize, kids) || kids == 0) {
                Fail(a_err, "Clone3D result has no children (no geometry?)");
                a_out.reset();
                return false;
            }
        }
        auto* obj = a_out.get();
        if (GetParent(obj) != nullptr || GetChildIndex(obj) != 0xFFFF) {
            Fail(a_err, "Clone3D result unexpectedly parented — refusing to hand it out");
            a_out.reset();
            return false;
        }
        if (GetRefCount(obj) != 1) {
            Fail(a_err, std::format("Clone3D result refcount {} != 1", GetRefCount(obj)));
            a_out.reset();
            return false;
        }
        for (std::size_t i = 0; i < before.size(); ++i) {
            std::uintptr_t now = 0;
            (void)Util::SafeReadQword(Addr(a_form) + 0x260 + i * 8, now);
            if (now != before[i]) {
                Fail(a_err, std::format("base form mutated at +0x{:X} — refusing", 0x260 + i * 8));
                a_out.reset();
                return false;
            }
        }
        return true;
    }
}
