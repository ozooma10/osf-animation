#include "API/OSFOverlayAPI.h"
#include "API/OverlayAPIControl.h"

#include "Animation/GraphManager.h"
#include "Overlay/OverlayService.h"

#include <atomic>

namespace OSF::API
{
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
				result.disposition, result.reason };
		}

		OSFOverlayRequestResult RequestStation(std::int32_t a_handle,
			const char* a_station, std::uint64_t a_token) override
		{
			const auto result = Overlay::OverlayService::GetSingleton().RequestStation(
				a_handle, a_station ? a_station : "", a_token);
			return { sizeof(OSFOverlayRequestResult), result.disposition, result.reason };
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
