// OSFSceneAPI.h - OSF Animation native C++ API. Copyable single header; link nothing.
// Mutating calls (start/stop/stage/navigate, Play/Stop/SetSpeed) must run on the game thread. Queries are
// locked and callable from any thread; prefer the int32 handle over actor pointers for off-thread polling.

#pragma once

#include "RE/Starfield.h"
#include "REX/W32/KERNEL32.h"

#include <cstdint>
#include <string>

namespace OSF::API
{
	// Packed (MAJOR << 16) | MINOR. MINOR bumps append vmethods or OSFStartOptions fields.
	inline constexpr std::uint32_t kOSFSceneAPIVersion = (2u << 16) | 0u;
	inline constexpr std::uint32_t kOSFSceneAPIMajor   = kOSFSceneAPIVersion >> 16;
	inline constexpr std::uint32_t kOSFSceneAPIMinor   = kOSFSceneAPIVersion & 0xFFFFu;

	inline constexpr bool SceneRequestSupported(std::uint32_t requested, std::uint32_t provided) noexcept
	{
		return (requested >> 16) == (provided >> 16) &&
			(requested & 0xFFFFu) <= (provided & 0xFFFFu);
	}

	inline constexpr const wchar_t* kOSFModuleName     = L"OSF Animation.dll";
	inline constexpr const char*    kRequestExportName = "OSF_RequestSceneAPI";
	inline constexpr const char*    kReportMinimumVersionExportName = "OSF_ReportMinimumVersion";

	// Result of ReportMinimumVersion; the export is independent of the scene-interface minor.
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

	// Call once from kPostDataLoad. If OSF predates the export, reads its version metadata and warns on the HUD itself.
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

		// Fallback for OSF builds without the export: SFSEPlugin_Version's two-field prefix, REL::Version packing.
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

	// Per-start overrides. Tri-states: 1 = on, 0 = off, anything else = inherit the scene/content default.
	struct OSFStartOptions
	{
		std::uint32_t size = sizeof(OSFStartOptions);  // stamped by your compiler; OSF reads only this many bytes

		std::int32_t hideApparelMode         = -1;    // hide apparel
		std::int32_t playerInputLockMode    = -1;    // player input lock
		std::int32_t sceneControlsMode = -1;    // scene controls; 0 = revoke advance/navigate/end
		std::int32_t fadeMode          = -1;    // screen fade

		char  camera[64] = {};                  // "" = inherit, e.g. "thirdperson_hold"
		float loopScale  = 1.0f;                // <=0/NaN -> 1, clamped to 20

		float        speed      = 1.0f;         
		float        blendIn    = 0.4f;         
		std::int32_t startStage = 0;           

		bool  hasAnchor        = false;         // world-anchor at anchorX/Y/Z instead of actor[0]; for furniture use anchorRef
		float anchorX          = 0.0f;
		float anchorY          = 0.0f;
		float anchorZ          = 0.0f;
		float anchorHeadingRad = 0.0f;          // hasAnchor only

		// Furniture/object anchor, using its facing; overrides hasAnchor. Anchor-bound scenes require their furniture
		// and apply the authored offset; free scenes world-anchor here. nullptr = none.
		RE::TESObjectREFR* anchorRef = nullptr;

		// 1 = follow actors (no teleport or root/heading pin; vanilla camera untouched),
		// 0 = force anchored, else inherit.
		std::int32_t followActorMode = -1;


	};

	// Matchmaking input for FindSceneByQuery / StartSceneByQuery and engine-services ReplaceSceneByQuery.
	// Arrays are borrowed for the call only. A null array needs a zero count; null or "" entries are skipped.
	struct OSFSceneQuery
	{
		std::uint32_t size = sizeof(OSFSceneQuery);  // stamped by your compiler; OSF reads only this many bytes

		std::int32_t requireAnchor = 0;          // 1 = only anchor-bound scenes `anchor` satisfies; else free scenes too

		const char* const* allOf = nullptr;      // every tag (case-insensitive)
		const char* const* anyOf = nullptr;      // at least one tag; empty = ignored
		const char* const* noneOf = nullptr;     // no tag
		// roles[i] restricts actor i to the role of that name (case-insensitive). Hard: no fallback. roleCount is 0
		// (unconstrained) or the actor count; a null or "" entry accepts any role. For a handoff, actor i is the scene's
		// current participant i (GetSceneParticipants order).
		const char* const* roles = nullptr;
		// Furniture/object anchor. Filters anchor-bound scenes and places the match. For starts it must equal
		// OSFStartOptions.anchorRef when that is also set.
		RE::TESObjectREFR* anchor = nullptr;

		std::uint32_t allOfCount = 0;
		std::uint32_t anyOfCount = 0;
		std::uint32_t noneOfCount = 0;
		std::uint32_t roleCount = 0;
	};

	inline constexpr std::uint32_t kOSFSceneMatchMaxActors = 8;

	// FindSceneByQuery output, indexed by the caller's actor order.
	struct OSFSceneMatch
	{
		std::uint32_t size = sizeof(OSFSceneMatch);  // must cover this whole struct
		std::uint32_t actorCount = 0;

		char nodeId[256] = {};
		// roleIndex[i] = declared role index actor i binds to. Start positionally by ordering actors by this index,
		// or by name with roles[] via StartSceneRoles when the scene's role names are distinct.
		std::int32_t roleIndex[kOSFSceneMatchMaxActors] = {};
		char         roles[kOSFSceneMatchMaxActors][64] = {};  // declared role name; "" for an anonymous role
	};

	// Scene-event bits, shared with OSF.psc EVENT_*; OR together for a callback mask.
	namespace SceneEventType
	{
		inline constexpr std::int32_t kNodeEnter = 0x01;
		inline constexpr std::int32_t kNodeExit = 0x02;
		inline constexpr std::int32_t kCue = 0x04;
		inline constexpr std::int32_t kAction = 0x08;
		inline constexpr std::int32_t kSceneEnd = 0x10;
		inline constexpr std::int32_t kSceneBegin = 0x20;
		inline constexpr std::int32_t kNodeTransition = 0x40; // node=new global ID, edge=previous global ID
		inline constexpr std::int32_t kAll = 0xFFFF;
	}

	// Borrowed view: the struct and its strings are valid only during the callback.
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
		const char* anchor = "";  // named track position (enter/exit/end), not a world anchor
		std::int32_t result = 0;
		std::uint32_t reserved = 0; // reserved; initialize to zero
		std::int32_t cause = -1; // SCENE_END only, completed=0 / cancelled=1 / interrupted=2
		// monotonic mark-detection ns, in services AudioClockNs domain. Zero for non-timed
		// events. Preserved across the frame queue; not an authored due time or clip fraction.
		std::uint64_t detectedNs = 0;
	};

	// Runs synchronously on the dispatch thread (game thread for authored events), before the Papyrus event
	// is queued. May unregister itself; must not throw or retain the payload.
	using OSFSceneEventCallback = void (*)(
		const OSFSceneEvent* a_event, void* a_context);

	// Handles are int32 generational tokens (0 = failure / dead), never raw pointers.
	struct IOSFSceneAPI
	{
		virtual std::uint32_t GetInterfaceVersion() = 0;

		// Plugin semver. Any thread.
		virtual void GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) = 0;

		// True once playback hooks are installed and verified. Any thread.
		virtual bool IsReady() = 0;

		// Game thread; returns a handle, 0 on failure. a_actors in role order; null or already-busy actors fail.
		// Anchor-bound (furniture) scenes require OSFStartOptions.anchorRef; others co-locate at actor[0] or the world anchor.
		virtual std::int32_t StartScene(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId, const OSFStartOptions& a_opts) = 0;

		// a_roles[i] names a_actors[i]'s role (equal counts). Game thread.
		virtual std::int32_t StartSceneRoles(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId, const char* const* a_roles, std::uint32_t a_roleCount, const OSFStartOptions& a_opts) = 0;

		// One synthetic node; a_files[i] ("path.glb:animId" allowed) plays on a_actors[i]. Game thread.
		virtual std::int32_t StartSceneFiles(RE::Actor* const* a_actors, const char* const* a_files, std::uint32_t a_count, const OSFStartOptions& a_opts) = 0;

		// Game thread.
		virtual bool StopScene(std::int32_t a_handle) = 0;
		virtual bool StopSceneForActor(RE::Actor* a_actor) = 0;

		// Game thread.
		virtual bool SetStage(std::int32_t a_handle, std::int32_t a_stage) = 0;
		virtual bool Advance(std::int32_t a_handle) = 0;
		virtual bool Navigate(std::int32_t a_handle, const char* a_edgeId) = 0;

		// Game thread, except GetSpeed (any thread).
		virtual bool  Play(RE::Actor* a_actor, const char* a_file, const char* a_animId) = 0;
		virtual bool  Stop(RE::Actor* a_actor) = 0;
		virtual bool  SetSpeed(RE::Actor* a_actor, float a_speed) = 0;
		virtual float GetSpeed(RE::Actor* a_actor) = 0;

		// Any thread.
		virtual bool         IsPlaying(RE::Actor* a_actor) = 0;
		virtual std::int32_t GetSceneForActor(RE::Actor* a_actor) = 0;
		virtual std::int32_t GetSceneStage(std::int32_t a_handle) = 0;  // current node timeline segment; -1 for a stale handle

		// Writes up to a_cap actors into caller-owned a_out; returns the total. Ended handles keep their roster briefly
		// so end handlers can read it. Touch the actors only on the game thread.
		virtual std::uint32_t GetSceneParticipants(std::int32_t a_handle,
			RE::Actor** a_out, std::uint32_t a_cap) = 0;

		// Thread-safe; tokens persist until unregistered. a_sceneFilter 0 = all scenes, a_eventMask 0 = kAll.
		// Returns 0 for a null callback. Unregister waits for callbacks on other threads, never the calling one.
		virtual std::uint64_t RegisterSceneEventCallback(
			OSFSceneEventCallback a_callback, void* a_context,
			std::int32_t a_sceneFilter = 0,
			std::int32_t a_eventMask = SceneEventType::kAll) = 0;

		virtual bool UnregisterSceneEventCallback(std::uint64_t a_token) = 0;

		// Game thread. Matchmakes without side effects: tags, per-role filters, a_query.roles and the
		// anchor, then priority tier and weighted random, so repeated calls may differ. False = no fit or bad input
		// (null actor, more than kOSFSceneMatchMaxActors actors, undersized structs, a scene id over 255 bytes).
		virtual bool FindSceneByQuery(RE::Actor* const* a_actors, std::uint32_t a_count, const OSFSceneQuery& a_query,
			OSFSceneMatch& a_out) = 0;

		// Game thread; returns a handle, 0 on failure. Matchmakes like FindSceneByQuery and starts the
		// pick at its authored entry (startStage is ignored). a_query.anchor places the scene; a different non-null
		// a_opts.anchorRef is rejected. No HUD messages.
		virtual std::int32_t StartSceneByQuery(RE::Actor* const* a_actors, std::uint32_t a_count, const OSFSceneQuery& a_query,
			const OSFStartOptions& a_opts) = 0;

	protected:
		~IOSFSceneAPI() = default;  // OSF owns the singleton; never delete it.
	};

	using RequestSceneAPI_t = IOSFSceneAPI* (*)(std::uint32_t a_abiVersion);

	// Cache the result; each call does a module + symbol lookup.
	inline IOSFSceneAPI* RequestSceneAPI(std::uint32_t a_abiVersion = kOSFSceneAPIVersion) noexcept
	{
		const REX::W32::HMODULE mod = REX::W32::GetModuleHandleW(kOSFModuleName);
		if (!mod) {
			return nullptr;
		}
		const auto fn = reinterpret_cast<RequestSceneAPI_t>(REX::W32::GetProcAddress(mod, kRequestExportName));
		auto* api = fn ? fn(a_abiVersion) : nullptr;
		return api && SceneRequestSupported(a_abiVersion, api->GetInterfaceVersion()) ? api : nullptr;
	}
}
