// Optional engine services. Copy alongside OSFSceneAPI.h; link no Animation library.
#pragma once

#include "OSFSceneAPI.h"

namespace OSF::API
{
	inline constexpr std::uint32_t kOSFEngineAPIVersion = 0x00010000;
	enum class EngineEvent : std::uint32_t { WorldReset = 1, CatalogChanged = 2, ScenesChanged = 4, ContentReloaded = 8 };
	using EngineCallback = void (*)(EngineEvent, void*);
	using TextCallback = void (*)(const char*, void*);
	// Invoked exactly once for an accepted post at a later verified frame drain. execute=false means
	// world replacement canceled the command: release context only, without touching game objects.
	using FrameCallback = void (*)(void* context, bool execute);

	// Singleton table owned by Animation. All callbacks must catch exceptions and remain loaded
	// until unregistered / all accepted posts have completed (SFSE DLLs normally live to process exit).
	// No STL objects or allocator ownership cross this boundary. Text is borrowed until callback return.
	struct OSFEngineAPI
	{
		std::uint32_t size;
		std::uint32_t version;
		// Any thread. Always deferred; FIFO with engine commands. false retains ownership in caller.
		bool (*Post)(FrameCallback, void*);
		// Any thread. WorldReset runs synchronously at teardown; invalidate local generations only.
		// Other events may also originate off-thread. Dispatch game work with Post.
		// Unsubscribe waits for callbacks on other threads; self-unsubscribe is allowed.
		std::uint64_t (*Subscribe)(EngineCallback, void*);
		bool (*Unsubscribe)(std::uint64_t);
		// All remaining functions require the game thread (normally via Post).
		// Game thread. JSON schema 1, defined by OSFCatalog.h.
		// Catalog includes diagnostics/load errors. Callback runs once, inline, on success.
		bool (*ReadCatalog)(TextCallback, void*);
		// Starts the optional duration scan. No scan occurs merely because the engine is installed.
		void (*ScanDurations)();
		std::int32_t (*ReloadContent)();
		// Game thread. JSON array of accepting scene IDs; cast is positional role order.
		bool (*MatchCast)(RE::Actor* const*, std::uint32_t, TextCallback, void*);
		bool (*MatchAnchor)(RE::TESObjectREFR*, TextCallback, void*);
		// Game thread. JSON array: handle, id, stage, time, duration, speed, participant form IDs.
		bool (*ReadActiveScenes)(TextCallback, void*);
		bool (*SetSingleStage)(std::int32_t);
		bool (*SetSceneSpeed)(std::int32_t, float); // permits 0 (pause), unlike solo-clip SetSpeed
		float (*GetSceneSpeed)(std::int32_t); // -1 for an invalid/ended handle
		// Game thread, borrowed UTF-8 species/gender copied through callback.
		bool (*ReadActorIdentity)(RE::Actor*, TextCallback, void*);
		bool (*AnchorInFrontOfView)(RE::Actor*, float, OSFStartOptions*);
		// Shared camera/input mechanisms. The consumer owns each successful orbit acquisition and
		// must release it once; WorldReset revokes all holds. No browser lifetime is kept in the engine.
		bool (*AcquireOrbit)(const std::uint32_t*, std::uint32_t);
		void (*ReleaseOrbit)();
		bool (*ReadOrbitPose)(float* position3, float* forward3);
		void (*KickToThirdPerson)();
		void (*SetInteractionCursor)(bool);
		void (*InjectOrbitDelta)(float, float, float);
	};
	inline const OSFEngineAPI* RequestEngineAPI(std::uint32_t version = kOSFEngineAPIVersion) noexcept
	{
		const auto mod = REX::W32::GetModuleHandleW(kOSFModuleName);
		using Request = const OSFEngineAPI* (*)(std::uint32_t);
		const auto fn = mod ? reinterpret_cast<Request>(REX::W32::GetProcAddress(mod, "OSF_RequestEngineAPI")) : nullptr;
		return fn ? fn(version) : nullptr;
	}
}
