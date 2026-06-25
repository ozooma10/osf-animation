#pragma once

namespace OSF::Camera
{
	// Alternate scene camera postures beyond the default third-person hold.
	enum class CameraMode : std::uint8_t
	{
		kFreeFly,     // engine-native free cam (the `tfc` path): ToggleFreeCameraMode; engine routes WASD/mouse
		kVanityOrbit  // automatic slow orbit, needs no input (RE::CameraState::kAutoVanity)
	};

	// Standalone player-camera control. Two impositions compose:
	//   - THIRD-PERSON HOLD (SetStandaloneLock): forces third person and bounces the player back if they zoom into first person. 
	//   - STATE OVERRIDE (Acquire/SetLive/ReleaseStateOverride): drives the camera into a free-fly / vanity-orbit state and SUPPRESSES the hold's bounce while active.
	// A single prior-POV baseline is captured by whichever imposition engages first and restored when the last releases
	class CameraService
	{
	public:
		static CameraService& GetSingleton();

		// THIRD-PERSON HOLD. Ref-counted across owners; engages on the first holder, restores the prior POV on the last. Each true must be matched by one false.
		void SetStandaloneLock(bool a_enable);

		// STATE OVERRIDE. Acquire on the first holder captures the baseline POV and suppresses the third-person bounce; 
		// Release on the last restores the baseline (or hands the camera back to the hold if one is still held). 
		// SetLiveCameraState retargets the live camera WITHOUT touching the ref-count, so a scene can switch postures per node (e.g. vanity on the intro, free-fly on a later beat). 
		// Each Acquire must be matched by one Release.
		void AcquireStateOverride();
		void SetLiveCameraState(CameraMode a_mode);
		void ReleaseStateOverride();

		// Rides the update-hook call stream (job threads). POVSwitch stays enabled while the hold is held so vanilla scroll-zoom works;
		// if the player zooms/keys into first person, queue a game-thread bounce back to third person. 
		// Atomic early-out when no hold is held OR a state override is suppressing the bounce.
		void Tick();

		// Save/load teardown: drops every imposition without forcing a mode (the loaded save is authoritative, matching GraphManager::StopAll).
		void OnStopAll();

	private:
		// Capture the prior POV once, on the game thread, when the first imposition engages.
		void CaptureBaseline();
		// Restore the prior POV on the game thread, only if no imposition remains and the live camera differs from the baseline.
		void RestoreBaseline();

		// engine native freecam (`tfc`). ToggleFreeCameraMode enters kFreeWalk + seeds the pose from the current view + routes input
		void NativeFreeCamEnter();
		void NativeFreeCamExit();

		std::mutex lock;
		std::atomic<bool> holdArmed{ false };       // gates Tick: a third-person hold is active
		std::atomic<bool> suppressBounce{ false };  // a state override owns the camera — don't bounce
		std::atomic<bool> bouncePending{ false };
		bool baselineCaptured = false;
		bool baselineWasFirstPerson = false;
		int  holdCount = 0;      // third-person-hold holders (control lock + thirdperson_hold tracks)
		int  overrideCount = 0;  // state-override holders (free-fly / vanity-orbit scenes)

		std::atomic<bool> nativeFreeCamActive{ false };  // engine-native free cam is toggled on
	};
}
