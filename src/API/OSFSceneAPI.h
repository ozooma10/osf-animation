// ============================================================================
// OSFSceneAPI.h - OSF Animation native C++ API.
//
// Copyable SINGLE header. Drop it into your SFSE plugin; link NOTHING.
//
// THREADING NOTES:
// Mutating calls (StartScene, StartSceneRoles, StartSceneFiles, StopScene, StopSceneForActor, SetStage, Advance, Navigate, Play, Stop, SetSpeed) 
// MUST run on the GAME (main) thread: they synchronously read/write engine actor state and return immediately.
// 
// Read-only queries (GetInterfaceVersion, GetPluginVersion, IsReady, GetSpeed, IsPlaying, GetSceneForActor, GetSceneStage, GetSceneParticipants)
// are internally locked and callable from ANY thread - but OSF only compares pointer identity; for off-thread polling prefer the int32 handle.
// ============================================================================

#pragma once

#include "RE/Starfield.h"      // RE::Actor, RE::TESObjectREFR (from YOUR identical CommonLibSF)
#include "REX/W32/KERNEL32.h"  // REX::W32::GetModuleHandleW / GetProcAddress / HMODULE (no <Windows.h>)

#include <cstdint>
#include <string>

namespace OSF::API
{
	// Packed (MAJOR << 16) | MINOR. MAJOR breaks ABI; MINOR bumps on an appended vmethod or an appended OSFStartOptions field.
	inline constexpr std::uint32_t kOSFSceneAPIVersion = (1u << 16) | 3u;
	inline constexpr std::uint32_t kOSFSceneAPIMajor   = kOSFSceneAPIVersion >> 16;
	inline constexpr std::uint32_t kOSFSceneAPIMinor   = kOSFSceneAPIVersion & 0xFFFFu;

	inline constexpr const wchar_t* kOSFModuleName     = L"OSF Animation.dll";
	inline constexpr const char*    kRequestExportName = "OSF_RequestSceneAPI";
	inline constexpr const char*    kReportMinimumVersionExportName = "OSF_ReportMinimumVersion";

	// Result from ReportMinimumVersion. The report export is deliberately
	// independent of the scene-interface minor: a consumer can report its
	// requirement before requesting whatever scene ABI it needs.
	enum class MinimumVersionResult : std::uint32_t
	{
		kSupported = 0,
		kUpgradeRequired = 1,
		kUnavailable = 2,  // OSF is absent or has no readable version metadata
		kInvalidRequest = 3,
	};

	using ReportMinimumVersion_t = std::uint32_t (*)(
		const char* a_consumer, std::uint32_t a_major,
		std::uint32_t a_minor, std::uint32_t a_patch);

	// Call once from kPostDataLoad. New OSF builds centralize and deduplicate the
	// report. If the installed build predates the report export, the helper reads
	// its standard SFSE version metadata and emits the same HUD warning from the
	// consumer, so the version-mismatch path still works against older engines.
	inline MinimumVersionResult ReportMinimumVersion(
		const char* a_consumer, std::uint32_t a_major,
		std::uint32_t a_minor = 0, std::uint32_t a_patch = 0) noexcept
	{
		const REX::W32::HMODULE mod = REX::W32::GetModuleHandleW(kOSFModuleName);
		if (!mod) {
			return MinimumVersionResult::kUnavailable;
		}
		if (const auto fn = reinterpret_cast<ReportMinimumVersion_t>(
				REX::W32::GetProcAddress(mod, kReportMinimumVersionExportName))) {
			return static_cast<MinimumVersionResult>(
				fn(a_consumer, a_major, a_minor, a_patch));
		}

		// Compatibility fallback for OSF builds released before this export. Every
		// SFSE plugin exposes SFSEPlugin_Version; only its fixed two-field prefix is
		// needed here. The packed layout is REL::Version's public format.
		struct PluginVersionPrefix
		{
			std::uint32_t dataVersion;
			std::uint32_t pluginVersion;
		};
		const auto* metadata = reinterpret_cast<const PluginVersionPrefix*>(
			REX::W32::GetProcAddress(mod, "SFSEPlugin_Version"));
		if (!metadata) {
			return MinimumVersionResult::kUnavailable;
		}
		const std::uint32_t packed = metadata->pluginVersion;
		const std::uint32_t installedMajor = (packed >> 24) & 0xFFu;
		const std::uint32_t installedMinor = (packed >> 16) & 0xFFu;
		const std::uint32_t installedPatch = (packed >> 4) & 0xFFFu;
		const bool supported =
			installedMajor != a_major ? installedMajor > a_major :
			installedMinor != a_minor ? installedMinor > a_minor :
			installedPatch >= a_patch;
		if (supported) {
			return MinimumVersionResult::kSupported;
		}

		try {
			if (auto* source = RE::ShowHUDMessageEvent::GetEventSource()) {
				const std::string name = a_consumer && a_consumer[0] ? a_consumer : "An installed mod";
				const std::string text = name + " requires OSF Animation v" +
					std::to_string(a_major) + "." + std::to_string(a_minor) + "." +
					std::to_string(a_patch) + " or newer. Upgrade OSF Animation.";
				RE::ShowHUDMessageEvent event{};
				event.text = RE::BSFixedString(text.c_str());
				event.sound = RE::BSFixedString();
				event.canThrottle = true;
				event.isWarning = true;
				source->Notify(event);
			}
		} catch (...) {
			// Reporting must never take the consumer down.
		}
		return MinimumVersionResult::kUpgradeRequired;
	}

	// -------------------------------------------------------------------------
	// Per-start overrides. Tri-states: 1 = force on, 0 = force off, anything else (incl. -1) = inherit the scene definition / content-file default.
	// -------------------------------------------------------------------------
	struct OSFStartOptions
	{
		std::uint32_t size = sizeof(OSFStartOptions);  // cbSize: stamped by YOUR compiler; OSF reads only this many bytes

		std::int32_t stripMode         = -1;    // frozen ABI field -> StartOverrides::hideApparel
		std::int32_t lockPlayerMode    = -1;    // frozen ABI field -> StartOverrides::playerInputLock
		std::int32_t playerControlMode = -1;    // frozen ABI field -> StartOverrides::sceneControls (0 = revoke advance/navigate/end)
		std::int32_t fadeMode          = -1;    // -> StartOverrides::fade

		char  camera[64] = {};                  // "" = inherit; e.g. "thirdperson_hold" -> StartOverrides::camera
		float loopScale  = 1.0f;                // -> StartOverrides::loopScale (OSF sanitizes <=0/NaN->1, clamps to 20)

		float        speed      = 1.0f;         
		float        blendIn    = 0.4f;         
		std::int32_t startStage = 0;           

		bool  hasAnchor        = false;         // world-anchor at the pos below instead of co-locating at actor[0] (a pre-resolved WORLD anchor; for furniture use anchorRef)
		float anchorX          = 0.0f;          // -> AnchorOverride::pos.x
		float anchorY          = 0.0f;          // -> AnchorOverride::pos.y
		float anchorZ          = 0.0f;          // -> AnchorOverride::pos.z
		float anchorHeadingRad = 0.0f;          // RADIANS -> AnchorOverride::heading (the hasAnchor world anchor only)

		// Furniture/object to anchor at. For an anchor-BOUND scene OSF validates this is the required furniture and composes the scene's authored anchorOffset;
		// for a free scene it world-anchors there. Uses the ref's own facing. Takes precedence over hasAnchor. nullptr = none.
		RE::TESObjectREFR* anchorRef = nullptr;

		// APPENDED at MINOR 2 (OSF reads it only when your stamped `size` covers it).
		// Frozen ABI field -> StartOverrides::worldPlacement: 1 = follow actors — no teleport,
		// no per-frame root/heading pin (leaves the player's heading, and with it the vanilla
		// third-person camera, alone). 0 = force the anchored posture; else = inherit the scene's.
		std::int32_t inPlaceMode = -1;
	};

	// Scene-event bits. These values are shared with OSF.psc EVENT_* constants
	// and can be ORed together when registering a native callback.
	namespace SceneEventType
	{
		inline constexpr std::int32_t kNodeEnter = 0x01;
		inline constexpr std::int32_t kNodeExit = 0x02;
		inline constexpr std::int32_t kCue = 0x04;
		inline constexpr std::int32_t kAction = 0x08;
		inline constexpr std::int32_t kSceneEnd = 0x10;
		inline constexpr std::int32_t kSceneBegin = 0x20;
		inline constexpr std::int32_t kAll = 0xFFFF;
	}

	// Borrowed, immutable scene-event view delivered to OSFSceneEventCallback.
	// The struct and every string pointer remain valid ONLY for the duration of
	// the callback. Copy any data the consumer needs to retain.
	struct OSFSceneEvent
	{
		std::uint32_t size = sizeof(OSFSceneEvent);
		std::int32_t sceneHandle = 0;
		std::int32_t eventType = 0;
		const char* node = "";
		const char* edge = "";
		const char* cue = "";
		const char* actionType = "";
		RE::Actor* actor = nullptr;
		const char* role = "";
		std::int32_t loopIndex = -1;
		float time = -1.0f;
		const char* anchor = "";  // frozen ABI name: named track position (enter/exit/end), never a world anchor
		std::int32_t result = 0;
	};

	// Native callbacks are invoked synchronously on the scene-runtime dispatch
	// thread (the game thread for authored scene events), before OSF queues the
	// equivalent Papyrus callback. A callback may unregister itself. It must not
	// retain borrowed payload pointers or throw across the DLL boundary.
	using OSFSceneEventCallback = void (*)(
		const OSFSceneEvent* a_event, void* a_context);

	// -------------------------------------------------------------------------
	// Handles are int32 generational tokens (0 = failure / dead), never raw pointers.
	// -------------------------------------------------------------------------
	struct IOSFSceneAPI
	{
		virtual std::uint32_t GetInterfaceVersion() = 0;

		// Plugin semver. ANY thread.
		virtual void GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) = 0;

		// True once OSF's playback hooks are installed + verified. ANY thread
		virtual bool IsReady() = 0;

		// --- scene start --- GAME THREAD. Returns handle; 0 = failure. ---
		// a_actors in role-declaration order; a null actor / an actor already in a scene fails.
		// Free scenes co-locate at actor[0] (or at the OSFStartOptions WORLD anchor). 
		// Anchor-BOUND (furniture) scenes require OSFStartOptions.anchorRef set to the furniture: a missing/incompatible ref fails (0).
		virtual std::int32_t StartScene(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId, const OSFStartOptions& a_opts) = 0;

		// Bind actors to NAMED roles: a_roles[i] is the role for a_actors[i] (equal counts). --- GAME THREAD.
		virtual std::int32_t StartSceneRoles(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId, const char* const* a_roles, std::uint32_t a_roleCount, const OSFStartOptions& a_opts) = 0;

		// --- ad-hoc files scene (one synthetic holding node) --- GAME THREAD. ---
		// a_files[i] plays on a_actors[i] (equal counts). "path.glb:animId" spec supported.
		virtual std::int32_t StartSceneFiles(RE::Actor* const* a_actors, const char* const* a_files, std::uint32_t a_count, const OSFStartOptions& a_opts) = 0;

		// --- stop --- GAME THREAD. ---
		virtual bool StopScene(std::int32_t a_handle) = 0;
		virtual bool StopSceneForActor(RE::Actor* a_actor) = 0;

		// --- navigation --- GAME THREAD. ---
		virtual bool SetStage(std::int32_t a_handle, std::int32_t a_stage) = 0;
		virtual bool Advance(std::int32_t a_handle) = 0;
		virtual bool Navigate(std::int32_t a_handle, const char* a_edgeId) = 0;

		// --- solo clip --- Play/Stop/SetSpeed GAME THREAD; GetSpeed ANY THREAD. ---
		virtual bool  Play(RE::Actor* a_actor, const char* a_file, const char* a_animId) = 0;
		virtual bool  Stop(RE::Actor* a_actor) = 0;
		virtual bool  SetSpeed(RE::Actor* a_actor, float a_speed) = 0;
		virtual float GetSpeed(RE::Actor* a_actor) = 0;

		// --- queries --- ANY THREAD (internally locked, read-only). ---
		virtual bool         IsPlaying(RE::Actor* a_actor) = 0;
		virtual std::int32_t GetSceneForActor(RE::Actor* a_actor) = 0;
		virtual std::int32_t GetSceneStage(std::int32_t a_handle) = 0;  // linear scenes; -1 otherwise

		// Writes up to a_cap entries into a_out (caller-owned); returns the TOTAL participant count.
		// Recently ended handles retain their final roster through a bounded asynchronous-event
		// window, so an end handler can read who took part. Copy it during the callback; old ended
		// handles eventually expire. Actors may be mid-teardown, so touch them only on the game thread.
		virtual std::uint32_t GetSceneParticipants(std::int32_t a_handle,
			RE::Actor** a_out, std::uint32_t a_cap) = 0;

		// --- native scene events (APPENDED at MINOR 3) ---
		// Registration is thread-safe and process-lifetime: tokens survive world
		// replacement until explicitly unregistered. a_sceneFilter == 0 accepts
		// every scene; a_eventMask == 0 is treated as SceneEventType::kAll.
		// Returns a 64-bit generational token, or 0 for a null callback. Once
		// Unregister returns, callbacks running on other threads have finished;
		// self-unregistration never waits on the current invocation itself.
		virtual std::uint64_t RegisterSceneEventCallback(
			OSFSceneEventCallback a_callback, void* a_context,
			std::int32_t a_sceneFilter = 0,
			std::int32_t a_eventMask = SceneEventType::kAll) = 0;

		virtual bool UnregisterSceneEventCallback(std::uint64_t a_token) = 0;

	protected:
		~IOSFSceneAPI() = default;  // OSF owns the singleton; the consumer never deletes it.
	};

	using RequestSceneAPI_t = IOSFSceneAPI* (*)(std::uint32_t a_abiVersion);

	// FETCH ONCE and cache the result - do NOT call this per-frame/per-poll (it does a module + symbol lookup).
	inline IOSFSceneAPI* RequestSceneAPI(std::uint32_t a_abiVersion = kOSFSceneAPIVersion) noexcept
	{
		const REX::W32::HMODULE mod = REX::W32::GetModuleHandleW(kOSFModuleName);
		if (!mod) {
			return nullptr;  // OSF not installed/loaded.
		}
		const auto fn = reinterpret_cast<RequestSceneAPI_t>(REX::W32::GetProcAddress(mod, kRequestExportName));
		return fn ? fn(a_abiVersion) : nullptr;  // older OSF (no export) or MAJOR mismatch -> nullptr.
	}
}
