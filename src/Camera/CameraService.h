#pragma once

namespace OSF::Camera
{
	// Standalone player-camera lock forcing third person and bounces the player back if they zoom into first person while it's held. 
	// This is just the lock mechanism; the scene runtime decides when to apply it.
	class CameraService
	{
	public:
		static CameraService& GetSingleton();

		// Restores the prior POV on release. REF-COUNTED across owners (the scene control lock + the camera track lane both drive it)
		// engages on the first holder, restores only when the last releases. Each true must be matched by one false.
		void SetStandaloneLock(bool a_enable);

		// Rides the update-hook call stream (job threads). POVSwitch stays enabled while the lock is held so vanilla scroll-zoom works; 
		// if the player zooms/keys into first person, queue a game-thread bounce back to third person. Atomic early-out when no lock is held.
		void Tick();

		// Save/load teardown: drops the lock without forcing a mode (the loaded save is authoritative, matching GraphManager::StopAll).
		void OnStopAll();

	private:
		std::mutex lock;
		std::atomic<bool> playerSceneActive{ false };  // armed while a lock is held (gates Tick)
		std::atomic<bool> bouncePending{ false };
		bool standaloneActive = false;
		bool standaloneWasFirstPerson = false;
		int  holdCount = 0;  // # of owners holding the lock (engages at 1, restores at 0)
	};
}
