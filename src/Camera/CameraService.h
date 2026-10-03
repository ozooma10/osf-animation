#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace RE { class PlayerCamera; }

namespace OSF::Camera
{
	enum class CameraMode : std::uint8_t
	{
		kFreeFly,      // engine-native `tfc`; engine routes WASD/mouse
		kVanityOrbit,  // RE::CameraState::kAutoVanity
		kSceneOrbit    // self-driven orbit; we write the FreeFly transform
	};

	// Two impositions compose: the third-person hold (SetStandaloneLock) and the state override
	// (Acquire/Release), which suppresses the hold's first-person bounce. One prior-POV baseline is
	// captured by whichever engages first and restored when the last releases.
	class CameraService
	{
	public:
		static CameraService& GetSingleton();

		// Gates features that write runtime-verified PlayerCamera/FreeFly fields for this game build.
		void SetRawLayoutSupport(bool a_enabled);
		[[nodiscard]] bool RawLayoutSupport() const;

		// Ref-counted; each true must be matched by one false.
		void SetStandaloneLock(bool a_enable);

		void SeedThirdPersonZoom(float a_distance, bool a_snapCurrent = false);

		// One-shot first->third person kick; takes no hold. Skipped while a state override owns the camera.
		void KickToThirdPerson();

		// Ref-counted; each Acquire must be matched by one Release. SetLiveCameraState retargets without touching the count.
		void AcquireStateOverride();
		void SetLiveCameraState(CameraMode a_mode);
		void ReleaseStateOverride();

		// Consumed by the next scene-orbit enter; form IDs are resolved at enter time, after placement. Empty -> player.
		void SetOrbitFrameSubjects(std::vector<std::uint32_t> a_actorIds);

		// Player MMB/R3 toggle as one paired state-override hold. Game thread only.
		void ToggleFreeCam();

		// The player toggle lives outside the scene's undo ledger; called on input-grant release so it can't outlive its scene.
		void ForcePlayerFreeCamOff();

		// Absolute-world camera transforms are unusable while the player's ship is in space. Game thread only.
		[[nodiscard]] bool SceneOrbitAvailable() const;

		// Last pose DriveSceneOrbit wrote; false when not driving or the drive lock is busy (callers skip, don't stall).
		bool SceneOrbitPose(float (&a_pos)[3], float (&a_fwd)[3]);

		// Browser drag-to-look: OSF UI freezes game input, so with no scene camera the player can't move the camera.
		// Takes one state-override hold; idempotent per browser session. False when the orbit is unavailable.
		bool EnsureBrowseOrbit(std::vector<std::uint32_t> a_frameSubjects);
		void ReleaseBrowseOrbit();
		[[nodiscard]] bool BrowseOrbitHeld() const { return browseOrbitHeld.load(std::memory_order_relaxed); }

		void Tick();

		// Drops every imposition without forcing a mode; the loaded save is authoritative.
		void OnStopAll();

		// After OnStopAll: a load can land in a leaked alt camera state with no driver (stuck at the origin); force third person.
		void OnPostLoad();

	private:
		// Generation-gated; OnStopAll advances the epoch so tasks queued for the discarded world are no-ops.
		void QueueTask(std::function<void()> a_task);

		enum class PlayerFreeCamReturn : std::uint8_t
		{
			kNone,
			kSceneOrbit,
			kVanityOrbit
		};

		// Game thread, lock held.
		void CaptureBaseline(RE::PlayerCamera& a_camera);
		void RestoreBaseline();

		// ToggleFreeCameraMode enters kFreeWalk and sets renderer/input flags; scene-orbit then switches to driven kFreeFly.
		void NativeFreeCamEnter(bool a_gamepadPassthrough);
		void NativeFreeCamExit();
		void RestoreAfterPlayerFreeCam(PlayerFreeCamReturn a_mode);

		void DriveSceneOrbit();

		std::mutex lock;
		std::atomic<std::uint64_t> taskEpoch{ 1 };
		std::atomic<bool> holdArmed{ false };
		std::atomic<bool> suppressBounce{ false };
		bool baselineCaptured = false;
		bool baselineWasFirstPerson = false;
		std::uint32_t baselineStateIndex = 0xFFFFFFFFu;  // >= kTotal = unknown
		int  holdCount = 0;
		int  overrideCount = 0;

		std::atomic<bool> nativeFreeCamActive{ false };
		std::atomic<bool> playerFreeCamHeld{ false };
		std::atomic<PlayerFreeCamReturn> playerFreeCamReturn{ PlayerFreeCamReturn::kNone };
		std::atomic<bool> browseOrbitHeld{ false };

		std::atomic<bool> orbitDriving{ false };
		std::mutex        driveLock;              // serializes DriveSceneOrbit across job threads
		std::int64_t      lastDriveMs = 0;
		// Current values low-pass toward the input-driven targets.
		float             orbitAzimuth = 0.0f;    // radians about world Z
		float             orbitElevation = 0.0f;  // radians, + = above center
		float             orbitRadius = 0.0f;     // meters
		float             orbitTargetAzimuth = 0.0f;
		float             orbitTargetElevation = 0.0f;
		float             orbitTargetRadius = 0.0f;
		// Seeded once on enter (re-reading a pinned actor's position jitters), then panned by WASD.
		float             orbitCenterX = 0.0f;
		float             orbitCenterY = 0.0f;
		float             orbitCenterZ = 0.0f;    // torso height
		float             orbitCenterTargetX = 0.0f;
		float             orbitCenterTargetY = 0.0f;
		float             orbitCenterTargetZ = 0.0f;
		float             orbitFloorZ = 0.0f;  // fixed; vertical input must not drag it down
		// Guarded by driveLock. Any manual input cancels the reframe glide.
		bool              orbitReframeGlide = false;
		// Guarded by driveLock; validity resets on each enter so a new session can't serve the previous pose.
		bool              lastOrbitPoseValid = false;
		float             lastOrbitPos[3] = {};
		float             lastOrbitFwd[3] = {};
		std::vector<std::uint32_t> frameSubjects;  // guarded by driveLock
	};
}
