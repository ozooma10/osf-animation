#pragma once

namespace OSF::Camera
{
	// Alternate scene camera postures beyond the default third-person hold.
	enum class CameraMode : std::uint8_t
	{
		kFreeFly,      // engine-native free cam (the `tfc` path): ToggleFreeCameraMode; engine routes WASD/mouse
		kVanityOrbit,  // automatic slow orbit, needs no input (RE::CameraState::kAutoVanity)
		kSceneOrbit    // self-driven mouse-steered orbit around the scene center (we write the FreeFly transform)
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

		// Seeds the third-person camera's TARGET zoom so a thirdperson_hold doesn't open pinned on the player's back.
		void SeedThirdPersonZoom(float a_distance, bool a_snapCurrent = false);

		// STATE OVERRIDE. Acquire on the first holder captures the baseline POV and suppresses the third-person bounce;
		// Release on the last restores the baseline (or hands the camera back to the hold if one is still held).
		// SetLiveCameraState retargets the live camera WITHOUT touching the ref-count, so a scene can switch postures per node (e.g. vanity on the intro, free-fly on a later beat).
		// Each Acquire must be matched by one Release.
		void AcquireStateOverride();
		void SetLiveCameraState(CameraMode a_mode);
		void ReleaseStateOverride();

		// FRAME SEED for the next scene-orbit enter: the scene cast to frame and center (form IDs;
		// resolved to live positions on the game thread AT ENTER TIME, after placement has moved the
		// actors — not when the seed is set). Consumed by that enter. Without a seed (or when nothing
		// resolves) the orbit falls back to centering on the player.
		void SetOrbitFrameSubjects(std::vector<std::uint32_t> a_actorIds);

		// PLAYER-DRIVEN FREE CAM (the MMB toggle, routed from InputService::kFreecam). Toggles engine-native
		// free cam on/off as one paired state override: the first toggle Acquires + enters free-fly, the second
		// Releases (handing the camera back to any scene posture / third-person hold / baseline). Composes with
		// scene-driven overrides via the same ref count. Game-thread only (the verb dispatch posts it there).
		void ToggleFreeCam();

		// Force the player-driven free cam OFF if it's currently held (no-op otherwise). The MMB hold lives
		// OUTSIDE the scene's undo ledger, so without this it can outlive the scene that granted it — leaving
		// the player in free-fly (WASD drives the camera, not the character) after the scene ends. The scene
		// runtime calls this when the player's input grant is released, so a free cam can't survive its scene.
		void ForcePlayerFreeCamOff();

		// Rides the update-hook call stream (job threads). POVSwitch stays enabled while the hold is held so vanilla scroll-zoom works;
		// if the player zooms/keys into first person, queue a game-thread bounce back to third person. 
		// Atomic early-out when no hold is held OR a state override is suppressing the bounce.
		void Tick();

		// Save/load teardown: drops every imposition without forcing a mode (the loaded save is authoritative, matching GraphManager::StopAll).
		void OnStopAll();

		// Post-load recovery (TESLoadGameEvent backstop, AFTER OnStopAll): a load can complete with the
		// camera still parked in an alt state imposed before the load — the engine doesn't reset it, and
		// with the orbit driver stopped kFreeFly sits at a dead transform with no input routed to it
		// (camera stuck in the ground at the origin). A load never legitimately lands in one of these
		// states, so finding one here means a leaked imposition: force third person to hand it back.
		void OnPostLoad();

	private:
		// Capture the prior POV once, on the game thread, when the first imposition engages.
		void CaptureBaseline();
		// Restore the prior POV on the game thread, only if no imposition remains and the live camera differs from the baseline.
		void RestoreBaseline();

		// engine native freecam (`tfc`). ToggleFreeCameraMode enters kFreeWalk + seeds the pose from the current view + routes input
		void NativeFreeCamEnter();
		void NativeFreeCamExit();

		// SCENE ORBIT (self-driven): per-frame from Tick (job threads) — places the camera on a ring around
		// the scene center and aims it inward, writing the FreeFly state's transform. Mouse steers azimuth/
		// elevation (drained from InputService), W/S zoom; holds still with no input.
		void DriveSceneOrbit();

		std::mutex lock;
		std::atomic<bool> holdArmed{ false };       // gates Tick: a third-person hold is active
		std::atomic<bool> suppressBounce{ false };  // a state override owns the camera — don't bounce
		std::atomic<bool> bouncePending{ false };
		bool baselineCaptured = false;
		bool baselineWasFirstPerson = false;
		int  holdCount = 0;      // third-person-hold holders (control lock + thirdperson_hold tracks)
		int  overrideCount = 0;  // state-override holders (free-fly / vanity-orbit scenes)

		std::atomic<bool> nativeFreeCamActive{ false };  // engine-native free cam is toggled on
		std::atomic<bool> playerFreeCamHeld{ false };    // the MMB player toggle owns one state-override hold

		std::atomic<bool> orbitDriving{ false };  // gates Tick's scene-orbit self-drive
		std::mutex        driveLock;              // serializes DriveSceneOrbit across job threads
		std::int64_t      lastDriveMs = 0;        // wall-clock anchor for frame-rate-independent dt
		// Smoothed (rendered) orbit params + their input-driven targets. Input nudges the targets; each
		// frame the current values low-pass toward them, so look/zoom glide instead of stepping per tick.
		float             orbitAzimuth = 0.0f;    // radians about world Z (camera position around center)
		float             orbitElevation = 0.0f;  // radians, + = camera above the center looking down
		float             orbitRadius = 0.0f;     // meters from the scene center
		float             orbitTargetAzimuth = 0.0f;
		float             orbitTargetElevation = 0.0f;
		float             orbitTargetRadius = 0.0f;
		// Orbit center: seeded on enter (a pinned actor's live position wobbles per frame → jitter, so we
		// don't re-read it), then flown through the scene by WASD in DriveSceneOrbit.
		float             orbitCenterX = 0.0f;
		float             orbitCenterY = 0.0f;
		float             orbitCenterZ = 0.0f;    // torso height; floor ≈ this minus the torso offset
		// Cast to frame on the next orbit enter (SetOrbitFrameSubjects). Guarded by driveLock.
		std::vector<std::uint32_t> frameSubjects;
	};
}
