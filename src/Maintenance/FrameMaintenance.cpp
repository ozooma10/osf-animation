#include "Maintenance/FrameMaintenance.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Profiling/Profile.h"
#include "Profiling/ProfilerHost.h"
#include "UI/FadeService.h"
#include "UI/Subtitle.h"
#include "Util/NativeMainThreadQueue.h"
#include "REL/THook.h"
#ifdef OSF_ANIMATION_SMOKE
#include "Testing/AnimationSmoke.h"
#include "Testing/D3AudioProbe.h"
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
			// The profiler's frame boundary: OSF time between consecutive marks is one frame's cost.
			OSF_PROFILE_FRAME();
			{
				OSF_PROFILE_SCOPE("Frame.Maintenance");
#ifdef OSF_ANIMATION_SMOKE
				Testing::D3AudioProbe::FrameDrain();
#endif
				{
					OSF_PROFILE_SCOPE("Frame.Drain");
					Util::NativeMainThreadQueue::Drain();
				}
				// Playback time and pose stamping live in the AnimationManager/BGSModelNode::Update hooks.
				{
					OSF_PROFILE_SCOPE("Frame.Camera.Tick");
					Camera::CameraService::GetSingleton().Tick();
				}
				{
					OSF_PROFILE_SCOPE("Frame.Fade.Tick");
					UI::FadeService::GetSingleton().Tick();
				}
				{
					OSF_PROFILE_SCOPE("Frame.Subtitle.Tick");
					UI::Subtitle::Tick();
				}
				{
					OSF_PROFILE_SCOPE("Frame.GraphManager.MaintenanceTick");
					Animation::GraphManager::GetSingleton().MaintenanceTick();
				}
#ifdef OSF_ANIMATION_SMOKE
				Testing::AnimationSmoke::Tick();
#endif
			}
			OSF_PROFILE_HOST_POLL();
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
