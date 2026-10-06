#include "Maintenance/FrameMaintenance.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Scene/PlaybackPreviewService.h"
#include "UI/FadeService.h"
#include "UI/Subtitle.h"
#include "Util/NativeMainThreadQueue.h"
#include "REL/THook.h"
#ifdef OSF_ANIMATION_SMOKE
#include "Testing/AnimationSmoke.h"
#endif

#include <optional>

namespace OSF::Maintenance
{
	namespace
	{
		constexpr std::ptrdiff_t kMainUpdateCallOffset = 0x2D6;
		std::optional<REL::THook<void()>> g_frameHook;

		void FrameThunk() noexcept
		{
			(*g_frameHook)();
			Util::NativeMainThreadQueue::Drain();
			// Playback time and pose stamping live in the AnimationManager/BGSModelNode::Update hooks.
			Camera::CameraService::GetSingleton().Tick();
			UI::FadeService::GetSingleton().Tick();
			UI::Subtitle::Tick();
			Animation::GraphManager::GetSingleton().MaintenanceTick();
			Scene::PlaybackPreviewService::GetSingleton().Tick();
#ifdef OSF_ANIMATION_SMOKE
			Testing::AnimationSmoke::Tick();
#endif
		}
	}

	bool InstallFrameMaintenance()
	{
		const REL::ID mainLoop{ 99488 };
		g_frameHook.emplace("[Anim] Frame::MainUpdate", mainLoop, kMainUpdateCallOffset, &FrameThunk);
		if (!g_frameHook->Init() || !g_frameHook->Enable()) {
			REX::ERROR("[Boot] Main-loop frame hook could not be installed at 99488+0x{:X}", kMainUpdateCallOffset);
			return false;
		}
		REX::TRACE("[Boot] installed frame maintenance after Main update at 99488+0x{:X} (native target 99406)", kMainUpdateCallOffset);
		return true;
	}
}
