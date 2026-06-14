#pragma once

namespace OSF::Camera
{
	// Standalone player-camera lock used by the SAF-compat playback path (the
	// Play+Sync flow that has no managed Scene). Forces third person and bounces
	// the player back if they zoom into first person while held. Scene-integrated
	// camera policy lives in the OSF Intimacy scene engine, not here.
	class CameraService
	{
	public:
		static CameraService& GetSingleton();

		// Standalone camera lock: forces third person and keeps Tick() bouncing
		// the player back if they zoom into first person while held. Restores the
		// prior POV on release. Idempotent.
		void SetStandaloneLock(bool a_enable);

		// Rides the update-hook call stream (job threads). POVSwitch stays enabled
		// while the lock is held so vanilla scroll-zoom works; this is the guard
		// rail — if the player zooms/keys into first person, queue a game-thread
		// bounce back to third person. Atomic early-out when no lock is held.
		void Tick();

		// Save/load teardown: drops the lock without forcing a mode (the loaded
		// save is authoritative, matching GraphManager::StopAll).
		void OnStopAll();

	private:
		std::mutex lock;
		std::atomic<bool> playerSceneActive{ false };  // armed while a lock is held (gates Tick)
		std::atomic<bool> bouncePending{ false };
		bool standaloneActive = false;
		bool standaloneWasFirstPerson = false;
	};
}
