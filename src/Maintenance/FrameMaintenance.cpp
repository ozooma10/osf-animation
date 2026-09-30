#include "Maintenance/FrameMaintenance.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Scene/PlaybackPreviewService.h"
#include "Studio/StudioPreviewService.h"
#include "UI/FadeService.h"
#include "UI/Subtitle.h"
#include "Util/NativeMainThreadQueue.h"
#include "REL/THook.h"

#include <array>
#include <optional>

namespace OSF::Maintenance
{
	namespace
	{
		// Same UI_AdvanceActiveMenus call sites used by OSF UI. THook captures the current
		// target, so both plugins participate in the chain regardless of installation order.
		constexpr std::array<std::ptrdiff_t, 2> kAdvanceCallSites{ 0x228, 0x2A1 };
		using AdvanceHook = REL::THook<void*(void*, void*, void*, void*)>;
		std::array<std::optional<AdvanceHook>, 2> g_advanceHooks;

		void Update()
		{
			Util::NativeMainThreadQueue::Drain();
			// AnimationManager::Update still owns subdivided playback time; BGSModelNode::Update
			// owns pose stamping. This game-thread pass owns commands and service maintenance.
			Camera::CameraService::GetSingleton().Tick();
			UI::FadeService::GetSingleton().Tick();
			UI::Subtitle::Tick();
			Animation::GraphManager::GetSingleton().MaintenanceTick();
			Scene::PlaybackPreviewService::GetSingleton().Tick();
			Studio::MaintenanceTick();
		}

		template <std::size_t Index>
		void* AdvanceThunk(void* a_ui, void* a_functor1, void* a_functor2, void* a_arg4) noexcept
		{
			auto* result = (*g_advanceHooks[Index])(a_ui, a_functor1, a_functor2, a_arg4);
			Update();
			return result;
		}
	}

	bool InstallFrameMaintenance()
	{
		g_advanceHooks[0].emplace("[Anim] Frame::Advance0", RE::ID::UI::UpdateMenus, kAdvanceCallSites[0], &AdvanceThunk<0>);
		g_advanceHooks[1].emplace("[Anim] Frame::Advance1", RE::ID::UI::UpdateMenus, kAdvanceCallSites[1], &AdvanceThunk<1>);
		for (auto& hook : g_advanceHooks) {
			if (!hook->Init()) {
				REX::ERROR("[Boot] UI_AdvanceActiveMenus frame hook could not be initialized");
				return false;
			}
		}
		for (auto& hook : g_advanceHooks) {
			hook->Enable();
		}
		REX::TRACE("[Boot] installed frame maintenance on UI::UpdateMenus");
		return true;
	}
}
