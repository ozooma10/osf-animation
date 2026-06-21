#include "Camera/CameraService.h"

namespace OSF::Camera
{
	CameraService& CameraService::GetSingleton()
	{
		static CameraService instance;
		return instance;
	}

	void CameraService::SetStandaloneLock(bool a_enable)
	{
		if (a_enable) {
			{
				std::scoped_lock l{ lock };
				if (++holdCount != 1) {
					return;  // already held by another owner, ref-count only
				}
				standaloneActive = true;
				playerSceneActive.store(true, std::memory_order_relaxed);  // arm Tick's bounce
			}
			SFSE::GetTaskInterface()->AddTask([this]() {
				auto* camera = RE::PlayerCamera::GetSingleton();
				if (!camera) {
					return;
				}
				{
					std::scoped_lock l{ lock };
					standaloneWasFirstPerson = camera->IsInFirstPerson();
				}
				if (!camera->IsInThirdPerson()) {
					camera->ForceThirdPerson();
					REX::INFO("Player camera forced to third person for standalone lock");
				}
			});
		} else {
			bool restore = false;
			{
				std::scoped_lock l{ lock };
				if (holdCount == 0) {
					return;  // not held, nothing to release
				}
				if (--holdCount != 0) {
					return;  // still held by another owner
				}
				standaloneActive = false;
				restore = standaloneWasFirstPerson;
				playerSceneActive.store(false, std::memory_order_relaxed);
			}
			if (restore) {
				SFSE::GetTaskInterface()->AddTask([this]() {
					// Don't restore if the lock was re-acquired meanwhile.
					{
						std::scoped_lock l{ lock };
						if (standaloneActive) {
							return;
						}
					}
					auto* camera = RE::PlayerCamera::GetSingleton();
					if (camera && !camera->IsInFirstPerson()) {
						camera->ForceFirstPerson();
						REX::INFO("Player camera restored to first person after standalone lock");
					}
				});
			}
		}
	}

	void CameraService::OnStopAll()
	{
		std::scoped_lock l{ lock };
		standaloneActive = false;
		holdCount = 0;
		playerSceneActive.store(false, std::memory_order_relaxed);
	}

	void CameraService::Tick()
	{
		if (!playerSceneActive.load(std::memory_order_relaxed)) {
			return;
		}

		// State read off-thread is a benign pointer compare; the actual camera
		// mutation lands on the game thread via the task queue.
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		if (!camera->IsInFirstPerson()) {
			return;
		}

		if (bouncePending.exchange(true, std::memory_order_relaxed)) {
			return;
		}

		SFSE::GetTaskInterface()->AddTask([this]() {
			bouncePending.store(false, std::memory_order_relaxed);
			if (!playerSceneActive.load(std::memory_order_relaxed)) {
				return;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (camera && camera->IsInFirstPerson()) {
				camera->ForceThirdPerson();
				REX::INFO("Player camera bounced back to third person mid-lock");
			}
		});
	}
}
