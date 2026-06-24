#include "Camera/CameraService.h"

namespace OSF::Camera
{
	namespace
	{
		// Map an OSF posture to the Starfield camera state machine.
		RE::CameraState ToCameraState(CameraMode a_mode)
		{
			switch (a_mode) {
			case CameraMode::kVanityOrbit:
				return RE::CameraState::kAutoVanity;
			case CameraMode::kFreeFly:
			default:
				return RE::CameraState::kFreeFly;
			}
		}
	}

	CameraService& CameraService::GetSingleton()
	{
		static CameraService instance;
		return instance;
	}

	void CameraService::CaptureBaseline()
	{
		// Game thread: record the prior POV once so any imposition restores to it. 
		// A second engage (overlapping scene) finds baselineCaptured already set and keeps the original POV.
		SFSE::GetTaskInterface()->AddTask([this]() {
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			std::scoped_lock l{ lock };
			if (!baselineCaptured) {
				baselineWasFirstPerson = camera->IsInFirstPerson();
				baselineCaptured = true;
			}
		});
	}

	void CameraService::RestoreBaseline()
	{
		SFSE::GetTaskInterface()->AddTask([this]() {
			bool wantFirst = false;
			{
				std::scoped_lock l{ lock };
				// Re-acquired meanwhile, or another imposition still holds the camera, leave it (and keep the baseline for that holder to restore later).
				if (holdCount > 0 || overrideCount > 0) {
					return;
				}
				wantFirst = baselineWasFirstPerson;
				baselineCaptured = false;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			if (wantFirst) {
				if (!camera->IsInFirstPerson()) {
					camera->ForceFirstPerson();
					REX::INFO("Player camera restored to first person after scene camera release");
				}
			} else if (!camera->IsInThirdPerson()) {
				// Explicit because a released override leaves the live camera in an alt state, not third.
				camera->ForceThirdPerson();
				REX::INFO("Player camera restored to third person after scene camera release");
			}
		});
	}

	void CameraService::SetStandaloneLock(bool a_enable)
	{
		if (a_enable) {
			{
				std::scoped_lock l{ lock };
				if (++holdCount != 1) {
					return;  // already held by another owner — ref-count only
				}
				holdArmed.store(true, std::memory_order_relaxed);  // arm Tick's bounce
			}
			CaptureBaseline();
			// Force third person now — unless a state override is currently imposing an alt camera.
			SFSE::GetTaskInterface()->AddTask([this]() {
				if (suppressBounce.load(std::memory_order_relaxed)) {
					return;  // a state override owns the camera; don't fight it
				}
				auto* camera = RE::PlayerCamera::GetSingleton();
				if (camera && !camera->IsInThirdPerson()) {
					camera->ForceThirdPerson();
					REX::INFO("Player camera forced to third person for standalone lock");
				}
			});
		} else {
			bool released = false;
			{
				std::scoped_lock l{ lock };
				if (holdCount == 0) {
					return;  // not held, nothing to release
				}
				if (--holdCount != 0) {
					return;  // still held by another owner
				}
				released = true;
				holdArmed.store(false, std::memory_order_relaxed);
			}
			if (released) {
				RestoreBaseline();  // no-ops if a state override still holds the camera
			}
		}
	}

	void CameraService::AcquireStateOverride()
	{
		{
			std::scoped_lock l{ lock };
			if (++overrideCount != 1) {
				return;  // already held by another scene — ref-count only
			}
			suppressBounce.store(true, std::memory_order_relaxed);  // stop the hold's bounce
		}
		CaptureBaseline();
	}

	void CameraService::SetLiveCameraState(CameraMode a_mode)
	{
		const RE::CameraState state = ToCameraState(a_mode);
		SFSE::GetTaskInterface()->AddTask([this, state]() {
			// Drive the camera only while an override is held (a late task after release is a no-op).
			if (!suppressBounce.load(std::memory_order_relaxed)) {
				return;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;  // fail-soft: no camera, no change
			}
			camera->SetCameraState(state);
			REX::INFO("Player camera set to scene state {}", static_cast<std::uint32_t>(state));
		});
	}

	void CameraService::ReleaseStateOverride()
	{
		bool released = false;
		{
			std::scoped_lock l{ lock };
			if (overrideCount == 0) {
				return;  // not held
			}
			if (--overrideCount != 0) {
				return;  // still held by another scene
			}
			released = true;
			suppressBounce.store(false, std::memory_order_relaxed);
		}
		if (!released) {
			return;
		}
		if (holdArmed.load(std::memory_order_relaxed)) {
			// A third-person hold is still active — hand the camera back to it instead of restoring.
			SFSE::GetTaskInterface()->AddTask([this]() {
				if (suppressBounce.load(std::memory_order_relaxed)) {
					return;  // an override was re-acquired meanwhile
				}
				auto* camera = RE::PlayerCamera::GetSingleton();
				if (camera && !camera->IsInThirdPerson()) {
					camera->ForceThirdPerson();
					REX::INFO("Player camera handed back to third-person hold after state override");
				}
			});
		} else {
			RestoreBaseline();
		}
	}

	void CameraService::OnStopAll()
	{
		std::scoped_lock l{ lock };
		holdCount = 0;
		overrideCount = 0;
		baselineCaptured = false;
		holdArmed.store(false, std::memory_order_relaxed);
		suppressBounce.store(false, std::memory_order_relaxed);
	}

	void CameraService::Tick()
	{
		if (!holdArmed.load(std::memory_order_relaxed)) {
			return;  // no third-person hold active
		}
		if (suppressBounce.load(std::memory_order_relaxed)) {
			return;  // a state override owns the camera — don't bounce
		}

		// State read off-thread is a benign pointer compare; the actual camera mutation lands on the
		// game thread via the task queue.
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
			if (!holdArmed.load(std::memory_order_relaxed) || suppressBounce.load(std::memory_order_relaxed)) {
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
