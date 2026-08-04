// OSFOverlayAPI.h - copyable native C++ API for persistent actor overlay routes.
#pragma once

#include "RE/Starfield.h"
#include "REX/W32/KERNEL32.h"

#include <cstdint>

namespace OSF::API
{
	inline constexpr std::uint32_t kOSFOverlayAPIVersion = (1u << 16) | 0u;
	inline constexpr std::uint32_t kOSFOverlayAPIMajor = 1;
	inline constexpr std::uint32_t kOSFOverlayAPIMinor = 0;
	inline constexpr const wchar_t* kOSFOverlayModuleName = L"OSF Animation.dll";
	inline constexpr const char* kOverlayRequestExportName = "OSF_RequestOverlayAPI";

	enum class OverlayRequestDisposition : std::uint32_t
	{
		kAccepted = 0,
		kPending = 1,
		kRejected = 2
	};

	enum class OverlayReason : std::uint32_t
	{
		kNone = 0,
		kInvalidHandle = 1,
		kUnknownStation = 2,
		kNoPath = 3,
		kBusy = 4,
		kSceneBlocked = 5,
		kActorUnavailable = 6,
		kPlaybackFailed = 7,
		kHandoffRejected = 8,
		kOwnerInvalid = 9,
		kRouteUnknown = 10,
		kDispatchDeferred = 11
	};

	enum class OverlayEventType : std::uint32_t
	{
		kCommit = 1,
		kMarker = 2,
		kReached = 3,
		kFailed = 4,
		kSuspended = 5,
		kResumed = 6,
		kEnded = 7,
		kPropAttach = 8,
		kPropDestroy = 9
	};

	enum class OverlayPhase : std::uint32_t
	{
		kAtStation = 0,
		kTransitioning = 1,
		kFailed = 2
	};

	enum class OverlayBlocker : std::uint32_t
	{
		kNone = 0,
		kScene = 1,
		kActor3DUnavailable = 2
	};

	struct OSFOverlayRequestResult
	{
		std::uint32_t size = sizeof(OSFOverlayRequestResult);
		OverlayRequestDisposition disposition = OverlayRequestDisposition::kRejected;
		OverlayReason reason = OverlayReason::kInvalidHandle;
	};

	struct OSFOverlayBeginResult
	{
		std::uint32_t size = sizeof(OSFOverlayBeginResult);
		std::int32_t routeHandle = 0;
		OverlayRequestDisposition disposition = OverlayRequestDisposition::kRejected;
		OverlayReason reason = OverlayReason::kOwnerInvalid;
	};

	// Borrowed callback view. String pointers remain valid only during the callback.
	struct OSFOverlayEvent
	{
		std::uint32_t size = sizeof(OSFOverlayEvent);
		OverlayEventType type = OverlayEventType::kMarker;
		std::int32_t routeHandle = 0;
		RE::Actor* actor = nullptr;
		const char* route = "";
		const char* fromStation = "";
		const char* toStation = "";
		const char* transition = "";
		const char* marker = "";
		const char* prop = "";
		std::uint64_t requestToken = 0;
		std::uint32_t transitionGeneration = 0;
		OverlayRequestDisposition outcome = OverlayRequestDisposition::kAccepted;
		OverlayReason reason = OverlayReason::kNone;
	};

	// Return true to acknowledge a commit handoff. Results are ignored for informational events.
	using OSFOverlayCallback = bool (*)(const OSFOverlayEvent* a_event, void* a_context);

	struct OSFOverlayRouteState
	{
		// Borrowed query view. String pointers remain valid only until the next overlay mutation.
		std::uint32_t size = sizeof(OSFOverlayRouteState);
		std::int32_t routeHandle = 0;
		RE::Actor* actor = nullptr;
		const char* route = "";
		const char* reachedStation = "";
		const char* checkpointStation = "";
		const char* desiredStation = "";
		OverlayPhase phase = OverlayPhase::kAtStation;
		OverlayBlocker blocker = OverlayBlocker::kNone;
		std::uint64_t requestToken = 0;
		std::uint32_t transitionGeneration = 0;
	};

	struct IOSFOverlayAPI
	{
		virtual std::uint32_t GetInterfaceVersion() = 0;
		virtual bool IsReady() = 0;
		virtual std::uint64_t AcquireOwner(const char* a_pluginId, OSFOverlayCallback a_callback, void* a_context) = 0;
		virtual bool ReleaseOwner(std::uint64_t a_owner) = 0;
		virtual OSFOverlayBeginResult BeginRoute(std::uint64_t a_owner, RE::Actor* a_actor,
			const char* a_routeId, const char* a_initialStation) = 0;
		virtual OSFOverlayRequestResult RequestStation(std::int32_t a_handle,
			const char* a_station, std::uint64_t a_token) = 0;
		virtual bool EndRoute(std::int32_t a_handle, bool a_fade = true) = 0;
		virtual std::int32_t GetRouteForActor(RE::Actor* a_actor) = 0;
		virtual bool QueryRoute(std::int32_t a_handle, OSFOverlayRouteState& a_out) = 0;

	protected:
		~IOSFOverlayAPI() = default;
	};

	using RequestOverlayAPI_t = IOSFOverlayAPI* (*)(std::uint32_t);
	inline IOSFOverlayAPI* RequestOverlayAPI(std::uint32_t a_version = kOSFOverlayAPIVersion) noexcept
	{
		const auto module = REX::W32::GetModuleHandleW(kOSFOverlayModuleName);
		if (!module) return nullptr;
		const auto request = reinterpret_cast<RequestOverlayAPI_t>(
			REX::W32::GetProcAddress(module, kOverlayRequestExportName));
		return request ? request(a_version) : nullptr;
	}
}
