#include "UI/PortraitCapture.h"

#include "Util/Hooking.h"  // PrologueMatches (engine-call gate)
#include "Util/Mem.h"      // guarded reads of the live 3D tree

#include <array>
#include <cstring>
#include <string>

namespace OSF::UI::PortraitCapture
{
	namespace
	{
		namespace Mem = OSF::Util::Mem;

		// --- AddrLib IDs (proven 1.16.244 / v21, OSF RE PortraitProbe) ------------
		constexpr REL::ID kCaptureFrameToFile{ 39202 };    // (const char* pathNoExt, u32 fmt 2=PNG, u32 flags 0)
		constexpr REL::ID kActorUpdateAppearance{ 101306 };  // (actor, u8 arg1, u32 flags, u8 changeRace)

		// Entry-byte prologue gates (first 10 bytes on .244).
		constexpr std::array<std::uint8_t, 10> kCaptureGate{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10 };
		constexpr std::array<std::uint8_t, 10> kUpdateAppearanceGate{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54 };

		// RTTI vtable RVAs (.244).
		constexpr std::uintptr_t kMenuActorVtableRva = 0x4C7ED60;
		constexpr std::uintptr_t kFaceGenNodeVtableRva = 0x4C0A800;  // BSFaceGenNiNodeSkinned

		// Form / actor / 3D-tree offsets (from the probe + brief's readback recipe).
		constexpr std::ptrdiff_t kFormId = 0x28;         // TESForm::formID (u32)
		constexpr std::ptrdiff_t kFormType = 0x2E;       // TESForm::formType (u8)
		constexpr std::uint8_t   kFormTypeNPC = 0x32;    // TESNPC
		constexpr std::uint8_t   kFormTypeACHR = 0x4B;   // placed actor ref
		constexpr std::ptrdiff_t kRefBaseObject = 0x98;  // TESObjectREFR base (ACHR -> its TESNPC)
		constexpr std::ptrdiff_t kActorLoadedData = 0xB8;  // -> loadedData
		constexpr std::ptrdiff_t kLoadedData3D = 0x8;    // loadedData -> data3D root NiNode
		constexpr std::ptrdiff_t kActorLoadState = 0xFC; // (& 7) == 3 => 3D loaded
		constexpr std::ptrdiff_t kActorAppearanceHolder = 0x228;  // -> [+8] appearance/morph component
		constexpr std::ptrdiff_t kAppearanceDirtyWord = 0x582;    // u16, |= 4 to request a rebuild

		// NiNode children array (brief): data ptr @ +0x138, u16 count @ +0x142.
		constexpr std::ptrdiff_t kNiNodeChildren = 0x138;
		constexpr std::ptrdiff_t kNiNodeChildCount = 0x142;

		// MenuActor "init from NPC" virtual: [vtbl+0x7B8](this, TESNPC*, 1).
		constexpr std::ptrdiff_t kInitFromNpcVtblOff = 0x7B8;

		// --- timing (per-frame ticks, ~60/s) -------------------------------------
		constexpr int kMinFaceFrames = 45;    // floor: never capture in under ~0.75 s
		constexpr int kMaxFaceFrames = 720;   // ceiling: ~12 s then capture regardless (ship v1)
		constexpr int kSettleFrames = 24;     // face sig must hold this long to count as "done"
		constexpr int kPngWaitFrames = 90;    // ~1.5 s for the async render-graph PNG write
		constexpr int kRestoreFrames = 90;    // bounded grace to let the player's face rebuild

		enum class Phase
		{
			kIdle,
			kAwaitFace,
			kAwaitPng,
			kAwaitRestore,
		};

		// The single in-flight capture (one shared doll => one at a time). Main thread only.
		struct State
		{
			Phase                 phase = Phase::kIdle;
			std::uintptr_t        actor = 0;        // the live MenuActor being hijacked
			std::uintptr_t        origBase = 0;     // player's TESNPC, restored at the end
			std::uintptr_t        targetBase = 0;   // the portrait subject's TESNPC
			std::filesystem::path pngNoExt;         // engine appends ".png"
			DoneFn                done;
			int                   frames = 0;       // frames in the current phase
			std::uint64_t         baselineSig = 0;  // facegen signature before the swap
			std::uint64_t         lastSig = 0;
			int                   stable = 0;
			bool                  captured = false;
		};
		State g_st;

		// --- gates ----------------------------------------------------------------
		[[nodiscard]] bool GatesOk()
		{
			return Util::Hooking::PrologueMatches(kCaptureFrameToFile, kCaptureGate) &&
			       Util::Hooking::PrologueMatches(kActorUpdateAppearance, kUpdateAppearanceGate);
		}

		// --- live MenuActor ------------------------------------------------------
		// CreateMenuActor registers the doll under editor-ID "InventoryPaperDoll" while the
		// paperdoll screen is open; verify the primary vtable before trusting it.
		[[nodiscard]] std::uintptr_t FindLiveMenuActor()
		{
			auto* form = RE::TESForm::LookupByEditorID("InventoryPaperDoll");
			if (!form) {
				return 0;
			}
			const auto p = reinterpret_cast<std::uintptr_t>(form);
			return Mem::VtableRva(p) == kMenuActorVtableRva ? p : 0;
		}

		// Is a_actor still the same live MenuActor (menu not closed / doll not freed)?
		[[nodiscard]] bool ActorStillLive(std::uintptr_t a_actor)
		{
			return a_actor != 0 && Mem::VtableRva(a_actor) == kMenuActorVtableRva;
		}

		// --- facegen signature ----------------------------------------------------
		// A cheap identity for "which face is on the doll": the BSFaceGenNiNodeSkinned node
		// pointer folded with its child count. The brief's completion signal is "the facegen
		// node is a NEW pointer"; the count guards the rare pointer-reuse case. 0 = couldn't
		// read the tree (treated as unknown, never as a change).
		[[nodiscard]] std::uint64_t FaceSignature(std::uintptr_t a_actor)
		{
			const auto loaded = Mem::ReadPtr(a_actor + kActorLoadedData);
			if (loaded == 0) {
				return 0;
			}
			const auto root = Mem::ReadPtr(loaded + kLoadedData3D);
			if (root == 0 || !Mem::IsReadable(root + kNiNodeChildCount, 2)) {
				return 0;
			}
			const auto data = Mem::ReadPtr(root + kNiNodeChildren);
			const std::uint16_t count = Mem::ReadU16(root + kNiNodeChildCount);
			if (data == 0 || count == 0 || count > 256 || !Mem::IsReadable(data, count * 8ull)) {
				return 0;
			}
			for (std::uint16_t i = 0; i < count; ++i) {
				const auto child = Mem::ReadPtr(data + i * 8ull);
				if (child != 0 && Mem::VtableRva(child) == kFaceGenNodeVtableRva) {
					const std::uint32_t fgChildren = Mem::ReadU32(child + kNiNodeChildCount);  // its head-geom count
					return (static_cast<std::uint64_t>(child) << 8) ^ fgChildren;
				}
			}
			return 0;
		}

		// --- engine calls ---------------------------------------------------------
		// Swap the doll's base NPC via the MenuActor vfunc (re-derives race + archetype).
		bool SwapBase(std::uintptr_t a_actor, std::uintptr_t a_npc)
		{
			const auto vtbl = Mem::ReadPtr(a_actor);
			const auto fn = Mem::ReadPtr(vtbl + kInitFromNpcVtblOff);
			if (fn == 0 || !Mem::IsImagePtr(fn)) {
				return false;
			}
			using InitFn = void (*)(std::uintptr_t, std::uintptr_t, std::uint8_t);
			reinterpret_cast<InitFn>(fn)(a_actor, a_npc, 1);
			return true;
		}

		// Dirty the appearance component + rebuild the face from the current base. Mirrors the
		// Papyrus ChangeHeadPart native exactly: [actor+0x228 -> +8]+0x582 |= 4; UpdateAppearance.
		void RebuildFace(std::uintptr_t a_actor)
		{
			const auto holder = Mem::ReadPtr(a_actor + kActorAppearanceHolder);
			const auto comp = holder != 0 ? Mem::ReadPtr(holder + 8) : 0;
			if (comp != 0 && Mem::IsReadable(comp + kAppearanceDirtyWord, 2)) {
				const std::uint16_t dirty = Mem::ReadU16(comp + kAppearanceDirtyWord);
				*reinterpret_cast<std::uint16_t*>(comp + kAppearanceDirtyWord) =
					static_cast<std::uint16_t>(dirty | 4);
			}
			using UpdateFn = void (*)(std::uintptr_t, std::uint8_t, std::uint32_t, std::uint8_t);
			reinterpret_cast<UpdateFn>(kActorUpdateAppearance.address())(a_actor, 0, 0, 1);
		}

		void FireCapture(const std::filesystem::path& a_pngNoExt)
		{
			const std::string path = a_pngNoExt.string();
			using CapFn = void (*)(const char*, std::uint32_t, std::uint32_t);
			reinterpret_cast<CapFn>(kCaptureFrameToFile.address())(path.c_str(), 2 /*PNG*/, 0 /*LDR*/);
		}

		[[nodiscard]] std::uint8_t LoadState(std::uintptr_t a_actor)
		{
			return static_cast<std::uint8_t>(Mem::ReadU8(a_actor + kActorLoadState) & 7);
		}

		// --- teardown -------------------------------------------------------------
		// Restore of the player's face is done by the caller (the kAwaitRestore phase or
		// Abort) BEFORE this runs; Finish only releases state and reports. State is cleared
		// before the callback because the DoneFn may enqueue the next capture.
		void Finish(bool a_ok)
		{
			DoneFn done = std::move(g_st.done);
			g_st = State{};
			if (done) {
				done(a_ok);
			}
		}

		void Arm();  // fwd

		// Abort mid-flight: try to put the player's face back if we already swapped, then finish.
		void Abort()
		{
			if (g_st.origBase != 0 && ActorStillLive(g_st.actor)) {
				if (SwapBase(g_st.actor, g_st.origBase)) {
					RebuildFace(g_st.actor);
				}
			}
			REX::WARN("[UI] portrait capture aborted (paperdoll closed or tree lost)");
			Finish(false);
		}

		void Tick()
		{
			if (g_st.phase == Phase::kIdle) {
				return;
			}
			// The doll is freed the instant the paperdoll menu closes — bail before touching it.
			if (!ActorStillLive(g_st.actor)) {
				Abort();
				return;
			}
			g_st.frames++;

			switch (g_st.phase) {
			case Phase::kAwaitFace: {
				const std::uint64_t sig = FaceSignature(g_st.actor);
				g_st.stable = (sig != 0 && sig == g_st.lastSig) ? g_st.stable + 1 : 0;
				g_st.lastSig = sig;

				const bool changed = sig != 0 && sig != g_st.baselineSig;
				const bool settled = changed && g_st.stable >= kSettleFrames &&
				                     LoadState(g_st.actor) == 3 && g_st.frames >= kMinFaceFrames;
				const bool timedOut = g_st.frames >= kMaxFaceFrames;
				if (settled || timedOut) {
					FireCapture(g_st.pngNoExt);
					g_st.captured = true;
					REX::INFO("[UI] portrait: captured face for {:08X} after {} frames ({})",
						Mem::ReadU32(g_st.targetBase + kFormId), g_st.frames,
						settled ? "settled" : "timeout");
					g_st.phase = Phase::kAwaitPng;
					g_st.frames = 0;
				}
				break;
			}
			case Phase::kAwaitPng: {
				std::error_code ec;
				std::filesystem::path png = g_st.pngNoExt;
				png += ".png";
				const bool exists = std::filesystem::exists(png, ec);
				if (exists || g_st.frames >= kPngWaitFrames) {
					g_st.captured = exists;  // record whether the file actually landed
					// Restore the player's face before releasing the doll.
					if (g_st.origBase != 0 && SwapBase(g_st.actor, g_st.origBase)) {
						RebuildFace(g_st.actor);
					}
					g_st.phase = Phase::kAwaitRestore;
					g_st.frames = 0;
				}
				break;
			}
			case Phase::kAwaitRestore: {
				// Bounded: we don't strictly need the restore to finish before yielding, but
				// giving it a moment avoids leaving the visible doll mid-rebuild.
				if (g_st.frames >= kRestoreFrames) {
					const bool ok = g_st.captured;
					Finish(ok);
					return;  // Finish cleared state; do not re-arm
				}
				break;
			}
			default:
				break;
			}
			Arm();
		}

		void Arm()
		{
			SFSE::GetTaskInterface()->AddTask([]() { Tick(); });
		}

		// Resolve an ACHR ref or TESNPC formID to a TESNPC base pointer (guarded).
		[[nodiscard]] std::uintptr_t ResolveTargetNpc(RE::TESFormID a_id)
		{
			auto* form = RE::TESForm::LookupByID(a_id);
			if (!form) {
				return 0;
			}
			auto base = reinterpret_cast<std::uintptr_t>(form);
			const auto type = Mem::ReadU8(base + kFormType);
			if (type == kFormTypeACHR) {
				const auto npc = Mem::ReadPtr(base + kRefBaseObject);
				return (npc != 0 && Mem::ReadU8(npc + kFormType) == kFormTypeNPC) ? npc : 0;
			}
			return type == kFormTypeNPC ? base : 0;
		}
	}

	bool Available()
	{
		return GatesOk() && FindLiveMenuActor() != 0;
	}

	bool Busy()
	{
		return g_st.phase != Phase::kIdle;
	}

	bool Begin(RE::TESFormID a_refOrNpc, const std::filesystem::path& a_pngNoExt, DoneFn a_done)
	{
		if (Busy()) {
			return false;
		}
		if (!GatesOk()) {
			REX::WARN("[UI] portrait capture: engine prologue gate failed — not capturing (game patched?)");
			return false;
		}
		const std::uintptr_t actor = FindLiveMenuActor();
		if (actor == 0) {
			return false;  // paperdoll not open
		}
		const std::uintptr_t targetNpc = ResolveTargetNpc(a_refOrNpc);
		if (targetNpc == 0) {
			REX::DEBUG("[UI] portrait capture: {:08X} did not resolve to a TESNPC", a_refOrNpc);
			return false;
		}
		const std::uintptr_t origBase = Mem::ReadPtr(actor + kRefBaseObject);
		if (origBase == 0) {
			return false;  // can't guarantee restore without the player's base
		}

		g_st = State{};
		g_st.phase = Phase::kAwaitFace;
		g_st.actor = actor;
		g_st.origBase = origBase;
		g_st.targetBase = targetNpc;
		g_st.pngNoExt = a_pngNoExt;
		g_st.done = std::move(a_done);
		g_st.baselineSig = FaceSignature(actor);  // the player's current face, before the swap

		if (!SwapBase(actor, targetNpc)) {
			REX::WARN("[UI] portrait capture: base-swap vfunc unreadable — aborting");
			g_st = State{};
			return false;
		}
		RebuildFace(actor);
		REX::INFO("[UI] portrait capture: doll {:X} swapped player {:08X} -> {:08X}, rebuilding face",
			actor, Mem::ReadU32(origBase + kFormId), Mem::ReadU32(targetNpc + kFormId));
		Arm();
		return true;
	}
}
