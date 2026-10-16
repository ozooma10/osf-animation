// Optional engine services. Copy alongside OSFSceneAPI.h; link no Animation library.
#pragma once

#include "OSFSceneAPI.h"

namespace OSF::API
{
	// Services 3.4 appends matchmade handoff; 3.3 added same-cast handoff, 3.2 alignment, 3.1 minor negotiation.
	inline constexpr std::uint32_t kOSFEngineAPIVersion = 0x00030004;

	inline constexpr bool EngineRequestSupported(std::uint32_t requested, std::uint32_t provided) noexcept
	{
		return (requested >> 16) == (provided >> 16) && (requested & 0xFFFF) <= (provided & 0xFFFF);
	}
	inline constexpr bool EngineTableSupported(std::uint32_t requested, std::uint32_t provided,
		std::uint32_t size, std::size_t requiredSize) noexcept
	{
		return EngineRequestSupported(requested, provided) && size >= requiredSize;
	}

	enum class ClipProbeStatus : std::uint32_t { Missing, Unchanged, Available, Unprobeable };
	struct ClipProbeResult
	{
		std::uint32_t size{sizeof(ClipProbeResult)};
		ClipProbeStatus status{ClipProbeStatus::Missing};
		float seconds{-1};
		std::uint64_t bytes{};
		std::int64_t modified{};
	};
	// Exclusive external input channel. Zero restores normal scene input routing.
	enum InputRouting : std::uint32_t {
		SuppressRawMouse = 1, ConsumeThumbsticks = 2, ConsumeGamepadAfterDispatch = 4,
		AllowNativeFreeCamera = 8
	};
	enum class EngineEvent : std::uint32_t { WorldReset = 1, CatalogChanged = 2, ScenesChanged = 4, ContentReloaded = 8 };
	using EngineCallback = void (*)(EngineEvent, void*);
	using TextCallback = void (*)(const char*, void*);
	// Receives the immutable base catalog (schema 3), excluding all provider scenes.
	// Emit exactly one JSON array of OSFClipScene.h records, synchronously. No registry/API reentry.
	using ClipSceneProvider = void (*)(const char*, TextCallback emit, void* emitContext, void* context);
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
		// Unless marked any-thread, the remaining functions require the game thread (normally via Post).
		// Game thread. JSON schema 3, defined by OSFCatalog.h.
		// Catalog includes diagnostics/load errors. Callback runs once, inline, on success.
		bool (*ReadCatalog)(TextCallback, void*);
		// Any thread. Memory-only decoded duration snapshot (OSFCatalog.h); caller copies inline.
		bool (*ReadClipDurations)(TextCallback, void*);
		// Any thread; synchronous, potentially slow IO. Call from a consumer-owned worker.
		// Optional previous source identity avoids decoding an unchanged file. No cache writes or workers.
		bool (*ProbeClipDuration)(const char*, const char*, const ClipProbeResult*, ClipProbeResult*);
		std::int32_t (*ReloadContent)();
		// Game thread. JSON array of accepting scene IDs; cast is positional role order.
		bool (*MatchCast)(RE::Actor* const*, std::uint32_t, TextCallback, void*);
		bool (*MatchAnchor)(RE::TESObjectREFR*, TextCallback, void*);
		// Game thread. JSON array: handle, id, stage, time, duration, speed, participant form IDs.
		bool (*ReadActiveScenes)(TextCallback, void*);
		bool (*SetSingleStage)(std::int32_t);
		bool (*SetSceneSpeed)(std::int32_t, float); // scene-owned across nodes; 0 pauses, clamps to 100
		float (*GetSceneSpeed)(std::int32_t); // -1 for an invalid/ended handle
		// Game thread, borrowed UTF-8 species/gender copied through callback.
		bool (*ReadActorIdentity)(RE::Actor*, TextCallback, void*);
		// Game thread. World transform and optional navmesh position, each three floats in meters.
		bool (*ReadWorldAnchor)(RE::TESObjectREFR*, float* position3, float* heading);
		bool (*SnapWorldPosition)(RE::TESObjectREFR*, const float* position3, float* snapped3);
		// Shared camera/input mechanisms. The consumer owns each successful orbit acquisition and
		// must release it once; WorldReset revokes all holds. No browser lifetime is kept in the engine.
		bool (*AcquireOrbit)(const std::uint32_t*, std::uint32_t);
		void (*ReleaseOrbit)();
		bool (*ReadOrbitPose)(float* position3, float* forward3);
		void (*KickToThirdPerson)();
		void (*SetInputRouting)(std::uint32_t);
		// Raw mouse-equivalent units and wheel notches. Consumers convert their own pointer units.
		void (*InjectOrbitDelta)(float, float, float);
		// Game thread. A unique owner is rebuilt immediately and on each content reload. Providers
		// also run on the reload caller's thread; remain loaded until unregistered. Existing scene refs
		// pin their publication across replacement/removal. A malformed batch is rejected as a whole.
		bool (*RegisterClipSceneProvider)(const char* owner, ClipSceneProvider, void*);
		bool (*UnregisterClipSceneProvider)(const char* owner);
		// 3.2. Exact role name; delta4 = anchor-local x/y/z and heading in degrees.
		bool (*SetSceneAlignment)(std::int32_t, const char* role, const float* delta4);
		bool (*GetSceneAlignment)(std::int32_t, const char* role, float* delta4);
		bool (*SaveSceneAlignment)(std::int32_t);
		bool (*ResetSceneAlignment)(std::int32_t, const char* role);
		// 3.3. Keep handle/cast/ledger/speed. Only startStage and anchorRef are read from options.
		// Other options are ignored. Null options use the target's entry and retained anchor.
		bool (*ReplaceScene)(std::int32_t, const char* sceneId, const OSFStartOptions*);
		// roleNames[i] names the target role for participant i in the current scene's roster.
		bool (*ReplaceSceneRoles)(std::int32_t, const char* sceneId, const char* const* roleNames, std::uint32_t count);
		// 3.4. Game thread. Matchmade handoff for the same cast, excluding the current scene and keeping its world
		// placement mode. query->roles[i] constrains current participant i (GetSceneParticipants order); a non-null
		// query->anchor replaces the retained anchor reference. False leaves the current scene playing.
		bool (*ReplaceSceneByQuery)(std::int32_t, const OSFSceneQuery* query);
	};

	// Released 3.0 clients check exact equality. Give that request its original prefix/version.
	inline OSFEngineAPI LegacyEngineAPI(const OSFEngineAPI& current)
	{
		auto legacy = current;
		legacy.version = 0x00030000;
		legacy.size = static_cast<std::uint32_t>(offsetof(OSFEngineAPI, UnregisterClipSceneProvider) + sizeof(current.UnregisterClipSceneProvider));
		return legacy;
	}
	inline const OSFEngineAPI* RequestEngineAPI(std::uint32_t version = kOSFEngineAPIVersion) noexcept
	{
		const auto mod = REX::W32::GetModuleHandleW(kOSFModuleName);
		using Request = const OSFEngineAPI* (*)(std::uint32_t);
		const auto fn = mod ? reinterpret_cast<Request>(REX::W32::GetProcAddress(mod, "OSF_RequestEngineAPI")) : nullptr;
		const auto* api = fn ? fn(version) : nullptr;
		return api && EngineTableSupported(version, api->version, api->size, sizeof(OSFEngineAPI)) ? api : nullptr;
	}
}
