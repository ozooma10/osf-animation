#include "API/OSFOverlayAPI.h"
#include "API/OverlayAPIControl.h"

#include "Animation/GraphManager.h"
#include "Overlay/OverlayService.h"

#include <atomic>

namespace OSF::API
{
	namespace
	{
		OverlayRequestDisposition ToAPI(Overlay::RequestDisposition a_value)
		{
			switch (a_value) {
			case Overlay::RequestDisposition::kAccepted: return OverlayRequestDisposition::kAccepted;
			case Overlay::RequestDisposition::kPending: return OverlayRequestDisposition::kPending;
			case Overlay::RequestDisposition::kRejected: return OverlayRequestDisposition::kRejected;
			}
			return OverlayRequestDisposition::kRejected;
		}

		OverlayReason ToAPI(Overlay::RequestReason a_value)
		{
			switch (a_value) {
			case Overlay::RequestReason::kNone: return OverlayReason::kNone;
			case Overlay::RequestReason::kInvalidHandle: return OverlayReason::kInvalidHandle;
			case Overlay::RequestReason::kUnknownStation: return OverlayReason::kUnknownStation;
			case Overlay::RequestReason::kNoPath: return OverlayReason::kNoPath;
			case Overlay::RequestReason::kBusy: return OverlayReason::kBusy;
			case Overlay::RequestReason::kSceneBlocked: return OverlayReason::kSceneBlocked;
			case Overlay::RequestReason::kActorUnavailable: return OverlayReason::kActorUnavailable;
			case Overlay::RequestReason::kPlaybackFailed: return OverlayReason::kPlaybackFailed;
			case Overlay::RequestReason::kHandoffRejected: return OverlayReason::kHandoffRejected;
			case Overlay::RequestReason::kOwnerInvalid: return OverlayReason::kOwnerInvalid;
			case Overlay::RequestReason::kRouteUnknown: return OverlayReason::kRouteUnknown;
			case Overlay::RequestReason::kDispatchDeferred: return OverlayReason::kDispatchDeferred;
			}
			return OverlayReason::kPlaybackFailed;
		}
	}

	class OverlayAPIImpl final : public IOSFOverlayAPI
	{
	public:
		static OverlayAPIImpl& GetSingleton()
		{
			static OverlayAPIImpl singleton;
			return singleton;
		}

		std::uint32_t GetInterfaceVersion() override { return kOSFOverlayAPIVersion; }
		bool IsReady() override { return _ready.load(std::memory_order_acquire); }
		void MarkReady() { _ready.store(true, std::memory_order_release); }
		IOSFOverlayAPI* IfReady() { return IsReady() ? this : nullptr; }

		std::uint64_t AcquireOwner(const char* a_pluginId, OSFOverlayCallback a_callback, void* a_context) override
		{
			return Overlay::OverlayService::GetSingleton().AcquireOwner(a_pluginId ? a_pluginId : "", a_callback, a_context);
		}

		bool ReleaseOwner(std::uint64_t a_owner) override
		{
			return Overlay::OverlayService::GetSingleton().ReleaseOwner(a_owner);
		}

		OSFOverlayBeginResult BeginRoute(std::uint64_t a_owner, RE::Actor* a_actor,
			const char* a_routeId, const char* a_initialStation) override
		{
			const auto result = Overlay::OverlayService::GetSingleton().BeginRoute(a_owner, a_actor,
				a_routeId ? a_routeId : "", a_initialStation ? a_initialStation : "");
			return { sizeof(OSFOverlayBeginResult), result.handle,
				ToAPI(result.disposition), ToAPI(result.reason) };
		}

		OSFOverlayRequestResult RequestStation(std::int32_t a_handle,
			const char* a_station, std::uint64_t a_token) override
		{
			const auto result = Overlay::OverlayService::GetSingleton().RequestStation(
				a_handle, a_station ? a_station : "", a_token);
			return { sizeof(OSFOverlayRequestResult), ToAPI(result.disposition), ToAPI(result.reason) };
		}

		bool EndRoute(std::int32_t a_handle, bool a_fade) override
		{
			return Overlay::OverlayService::GetSingleton().EndRoute(a_handle, a_fade);
		}

		std::int32_t GetRouteForActor(RE::Actor* a_actor) override
		{
			return Overlay::OverlayService::GetSingleton().GetRouteForActor(a_actor);
		}

		bool QueryRoute(std::int32_t a_handle, OSFOverlayRouteState& a_out) override
		{
			if (a_out.size < sizeof(OSFOverlayRouteState)) return false;
			return Overlay::OverlayService::GetSingleton().QueryRoute(a_handle, a_out);
		}

	private:
		std::atomic<bool> _ready{ false };
	};

	void MarkOverlayReady()
	{
		OverlayAPIImpl::GetSingleton().MarkReady();
		REX::INFO("[API] native overlay API ready (ABI {:#x})", kOSFOverlayAPIVersion);
	}
}

extern "C" __declspec(dllexport) OSF::API::IOSFOverlayAPI* OSF_RequestOverlayAPI(std::uint32_t a_version)
{
	if ((a_version >> 16) != OSF::API::kOSFOverlayAPIMajor ||
		(a_version & 0xFFFFu) > OSF::API::kOSFOverlayAPIMinor) {
		return nullptr;
	}
	return OSF::API::OverlayAPIImpl::GetSingleton().IfReady();
}
