#include "Maintenance/FrameMaintenance.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Scene/PlaybackPreviewService.h"
#include "Studio/StudioPreviewService.h"
#include "UI/FadeService.h"
#include "UI/Subtitle.h"
#include "Util/NativeMainThreadQueue.h"

namespace OSF::Maintenance
{
	namespace
	{
		class FrameMaintenanceTask final : public SFSE::ITaskDelegate
		{
		public:
			void Run() override
			{
				// SFSE supplies this signal from rotating render workers and may outpace the game
				// thread. Keep at most one maintenance pass queued or running.
				if (_pending.exchange(true, std::memory_order_acq_rel)) {
					return;
				}

				const auto result = Util::NativeMainThreadQueue::Post(
					[this]() { RunOnMainThread(); },
					"[Anim] frame maintenance",
					[this]() { _pending.store(false, std::memory_order_release); });
				if (result == Util::NativeMainThreadQueue::PostResult::kUnavailable) {
					_pending.store(false, std::memory_order_release);
				}
			}

			void Destroy() override {}

		private:
			void RunOnMainThread() noexcept
			{
				try {
					// Keep graph sampling in AnimationManager::Update: its subdivided dt reports are
					// a separate clock contract.
					Camera::CameraService::GetSingleton().Tick();
					UI::FadeService::GetSingleton().Tick();
					UI::Subtitle::Tick();
					Animation::GraphManager::GetSingleton().MaintenanceTick();
					Scene::PlaybackPreviewService::GetSingleton().Tick();
					Studio::MaintenanceTick();
				} catch (const std::exception& e) {
					REX::ERROR("[Anim] frame maintenance threw '{}'; pass stopped", e.what());
				} catch (...) {
					REX::ERROR("[Anim] frame maintenance threw an unknown exception; pass stopped");
				}
				_pending.store(false, std::memory_order_release);
			}

			std::atomic_bool _pending{ false };
		};
	}

	bool InstallFrameMaintenance()
	{
		static bool installed = false;
		if (installed) {
			return true;
		}

		auto* tasks = SFSE::GetTaskInterface();
		if (!tasks) {
			REX::ERROR("[Boot] SFSE task interface unavailable — frame maintenance not installed");
			return false;
		}

		static FrameMaintenanceTask task;
		tasks->AddPermanentTask(&task);
		installed = true;
		REX::TRACE("[Boot] installed coalesced main-thread maintenance producer");
		return true;
	}
}
