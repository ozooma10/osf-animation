#include "Camera/CameraService.h"

#include "Input/InputService.h"

#include <algorithm>
#include <chrono>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace OSF::Camera
{
	namespace
	{
		constexpr std::uint32_t kNativeFreeCamState = 13;  // RE::CameraState::kFreeWalk (== plain `tfc`)
		using ToggleFreeCam_t = void (*)(RE::PlayerCamera*, std::uint32_t, bool);

		void NativeToggleFreeCam()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}

			camera->ToggleFreeCameraMode(kNativeFreeCamState, false);
		}

		// Free-cam INPUT CONTEXT. The console `tfc` handler pushes this right after ToggleFreeCameraMode (and pops it on exit). 
		using InputCtx_t = void (*)(void*, std::uint32_t, void*);
		constexpr std::uint32_t kFreeCamInputCtxId = 0x13;

		void* FreeCamInputMgr()
		{
			static REL::Relocation<void**> slot{ RE::ID::FreeCameraInputContext::Manager };
			return *slot;
		}

		void* FreeCamInputDescriptor()
		{
			static REL::Relocation<std::uintptr_t> desc{ RE::ID::FreeCameraInputContext::Descriptor };
			return reinterpret_cast<void*>(desc.address());  // the handler passes &descriptor (lea r8)
		}

		void PushFreeCamInputContext()
		{
			void* mgr = FreeCamInputMgr();
			if (!mgr) {
				return;
			}
			static REL::Relocation<InputCtx_t> push{ RE::ID::FreeCameraInputContext::PushContext };
			push(mgr, kFreeCamInputCtxId, FreeCamInputDescriptor());
			REX::INFO("Native free cam: input context pushed");
		}

		void PopFreeCamInputContext()
		{
			void* mgr = FreeCamInputMgr();
			if (!mgr) {
				return;
			}
			static REL::Relocation<InputCtx_t> pop{ RE::ID::FreeCameraInputContext::PopContext };
			pop(mgr, kFreeCamInputCtxId, FreeCamInputDescriptor());
			REX::INFO("Native free cam: input context popped");
		}

		// --- Scene-orbit self-drive. We place the camera on a ring around the scene center and aim it
		// inward, writing the FreeFly state's transform each frame (the engine renders from +0x70/+0x7c).
		// State @ PlayerCamera::cameraStates[kFreeFly]; pos = NiPoint3 world METERS @+0x70, orientation =
		// NiQuaternion (w,x,y,z) @+0x7c (OSF RE camera.state_machine, in-game proven).
		constexpr std::ptrdiff_t kFreeFlyPosOffset = 0x70;
		constexpr std::ptrdiff_t kFreeFlyRotOffset = 0x7c;

		// --- Third-person START ZOOM seed. ALL THREE values are UNKNOWN until reverse-engineered in-game
		// (osf-re camera.state_machine): the F4 layout (ThirdPersonState::targetZoomOffset @0xE8) does NOT
		// transfer — SF's TESCamera base is 0x48 vs F4's 0x38, so the whole derived struct is shifted/re-laid.
		// Discover via the dump-and-watch + single-scroll multi-frame ramp probe (the float that JUMPS on a
		// scroll is target; the one that RAMPS is current — leave current alone so the engine glides to target).
		// While the offset is 0x0, SeedThirdPersonZoom is a HARD NO-OP and never writes. Fill all three to arm it.
		constexpr std::ptrdiff_t kThirdPersonTargetZoomOffset = 0x0;  // <-- RE: float, the zoom SETPOINT (target)
		constexpr float          kThirdPersonZoomMin = 0.0f;          // <-- RE: closest safe seed (keep player's head un-culled)
		constexpr float          kThirdPersonZoomMax = 1.0f;          // <-- RE: farthest third-person zoom (axis max)

		// Orbit framing + control tunables (calibration expected on first run).
		constexpr float kOrbitCenterFwd = 0.5f;      // meters ahead of the player toward the partner (~pair midpoint)
		constexpr float kOrbitCenterUp = 1.0f;       // meters above the player's feet (torso height)
		constexpr float kOrbitFloorMargin = 0.3f;    // keep the camera at least this far above the floor
		constexpr float kOrbitDefaultRadius = 3.5f;  // meters
		constexpr float kOrbitDefaultElev = 0.32f;   // radians above the center (~18°)
		constexpr float kOrbitMinRadius = 1.0f;
		constexpr float kOrbitMaxRadius = 12.0f;
		constexpr float kOrbitMouseSens = 0.004f;    // radians per raw mouse unit
		constexpr float kOrbitPanSpeed = 5.0f;       // meters/sec the WASD keys fly the orbit center through the scene
		constexpr float kOrbitWheelStep = 0.6f;      // meters of radius change per mouse-wheel notch (zoom)
		constexpr float kOrbitElevLimit = 1.45f;     // ~83°, off the gimbal poles
		constexpr float kOrbitSmoothTime = 0.07f;    // low-pass time constant (s): smaller = snappier, larger = floatier

		bool KeyDown(int a_vk)
		{
			return (GetAsyncKeyState(a_vk) & 0x8000) != 0;
		}

		std::int64_t NowMs()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}

		// Unit look direction for yaw (about world Z) / pitch. Codebase heading convention
		// (Scene::PlacementToWorld): forward = (-sin yaw, cos yaw); +pitch tilts up (+Z).
		RE::NiPoint3 LookDirection(float a_yaw, float a_pitch)
		{
			const float cp = std::cos(a_pitch);
			return { -std::sin(a_yaw) * cp, std::cos(a_yaw) * cp, std::sin(a_pitch) };
		}

		RE::NiQuaternion OrientationQuat(float a_yaw, float a_pitch)
		{
			const RE::NiQuaternion qYaw = RE::NiQuaternion::AngleAxis(a_yaw, RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
			const RE::NiQuaternion qPitch = RE::NiQuaternion::AngleAxis(a_pitch, RE::NiPoint3{ 1.0f, 0.0f, 0.0f });
			return qYaw * qPitch;
		}

		void WriteFreeFlyTransform(void* a_state, const RE::NiPoint3& a_pos, float a_yaw, float a_pitch)
		{
			auto* base = reinterpret_cast<std::byte*>(a_state);
			auto* pos = reinterpret_cast<float*>(base + kFreeFlyPosOffset);
			pos[0] = a_pos.x;
			pos[1] = a_pos.y;
			pos[2] = a_pos.z;
			const RE::NiQuaternion q = OrientationQuat(a_yaw, a_pitch);
			auto* rot = reinterpret_cast<float*>(base + kFreeFlyRotOffset);
			rot[0] = q.w;
			rot[1] = q.x;
			rot[2] = q.y;
			rot[3] = q.z;
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

	void CameraService::SeedThirdPersonZoom(float a_distance)
	{
		if (a_distance <= 0.0f) {
			return;  // unset / non-positive = engine default; nothing to seed (default scenes unchanged).
		}
		// Hard no-op until the SF third-person zoom offset is pinned in-game. Never write at 0x0 — that
		// would clobber the state's vtable pointer. Filling the constant block above arms the seed.
		if (kThirdPersonTargetZoomOffset == 0) {
			REX::INFO("SeedThirdPersonZoom({}) requested, but the SF third-person zoom offset is not yet RE'd — no-op", a_distance);
			return;
		}
		SFSE::GetTaskInterface()->AddTask([this, a_distance]() {
			if (suppressBounce.load(std::memory_order_relaxed)) {
				return;  // a state override (free-fly / orbit) owns the camera — don't fight it.
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			// QCameraEquals(kThirdPerson) confirms third person is the LIVE state, so cameraStates[kThirdPerson]
			// is a valid ThirdPersonState (this is the same guard DriveSceneOrbit uses for kFreeFly, and it
			// subsumes a vtable/stateId check — the engine's own currentState == cameraStates[kThirdPerson]).
			// We're posted after the hold's ForceThirdPerson task, so the force has landed by here; if the engine
			// isn't in third person we skip rather than write into a cold/wrong state.
			if (!camera || !camera->QCameraEquals(RE::CameraState::kThirdPerson)) {
				return;
			}
			void* state = camera->cameraStates[RE::CameraState::kThirdPerson];
			if (!state) {
				return;
			}
			const float seed = std::clamp(a_distance, kThirdPersonZoomMin, kThirdPersonZoomMax);
			// Target ONLY — leave currentZoomOffset where it is so the engine eases current -> target (the glide).
			// Raw-offset write on the game thread, same idiom as WriteFreeFlyTransform.
			*reinterpret_cast<float*>(reinterpret_cast<std::byte*>(state) + kThirdPersonTargetZoomOffset) = seed;
			REX::INFO("Third-person start zoom seeded to {} (target-only; engine glides current -> target)", seed);
		});
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
		if (a_mode == CameraMode::kFreeFly) {
			NativeFreeCamEnter();  // engine-native free cam (the `tfc` path): engine owns camera + input
			return;
		}
		if (a_mode == CameraMode::kSceneOrbit) {
			// Self-driven orbit: enter the FreeFly state, then write its transform each frame in Tick.
			SFSE::GetTaskInterface()->AddTask([this]() {
				if (!suppressBounce.load(std::memory_order_relaxed)) {
					return;  // the override was released before this task ran
				}
				auto* camera = RE::PlayerCamera::GetSingleton();
				if (!camera) {
					return;
				}
				// Leave first person FIRST: SetCameraState(kFreeFly) from a first-person view keeps the
				// 1st-person model skin, so the player would see floating hands instead of the full body.
				// ForceThirdPerson swaps to the 3rd-person skeleton/skin before we seize the camera.
				if (camera->IsInFirstPerson()) {
					camera->ForceThirdPerson();
				}
				camera->SetCameraState(RE::CameraState::kFreeFly);  // we drive its +0x70/+0x7c transform
				auto* player = RE::PlayerCharacter::GetSingleton();
				{
					std::scoped_lock l{ driveLock };
					// Capture the orbit center ONCE — the player's torso, nudged toward the partner. A pinned
					// actor's live position wobbles each frame, so reading it per-frame would jitter the camera.
					const float        h = player ? player->data.angle.z : 0.0f;
					const RE::NiPoint3 loc = player ? player->data.location : RE::NiPoint3{};
					orbitCenterX = loc.x - std::sin(h) * kOrbitCenterFwd;
					orbitCenterY = loc.y + std::cos(h) * kOrbitCenterFwd;
					orbitCenterZ = loc.z + kOrbitCenterUp;
					orbitAzimuth = h;  // start framing the scene from the player's side
					orbitElevation = kOrbitDefaultElev;
					orbitRadius = kOrbitDefaultRadius;
					orbitTargetAzimuth = orbitAzimuth;  // targets start matched so there's no opening glide
					orbitTargetElevation = orbitElevation;
					orbitTargetRadius = orbitRadius;
					lastDriveMs = 0;
				}
				orbitDriving.store(true, std::memory_order_relaxed);
				Input::InputService::GetSingleton().SetMouseCapture(true);
				REX::INFO("Scene orbit camera engaged");
			});
			return;
		}
		// kVanityOrbit: a plain PlayerCamera state override (auto orbit, needs no input).
		SFSE::GetTaskInterface()->AddTask([this]() {
			if (!suppressBounce.load(std::memory_order_relaxed)) {
				return;  // the override was released before this task ran
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;  // fail-soft: no camera, no change
			}
			// Leave first person first (see kSceneOrbit) so the player renders as a full third-person body.
			if (camera->IsInFirstPerson()) {
				camera->ForceThirdPerson();
			}
			camera->SetCameraState(RE::CameraState::kAutoVanity);
			REX::INFO("Player camera set to vanity orbit");
		});
	}

	void CameraService::NativeFreeCamEnter()
	{
		SFSE::GetTaskInterface()->AddTask([this]() {
			if (!suppressBounce.load(std::memory_order_relaxed)) {
				return;  // the override was released before this task ran
			}
			if (nativeFreeCamActive.exchange(true, std::memory_order_relaxed)) {
				return;  // already on — toggling again would exit it
			}
			NativeToggleFreeCam();      // enter free cam; the engine seeds the pose from the current view
			PushFreeCamInputContext();  // the input context the console handler pushes
			REX::INFO("Native free camera entered (ToggleFreeCameraMode, state {})", kNativeFreeCamState);
		});
	}

	void CameraService::NativeFreeCamExit()
	{
		// Snapshot what we must hand the camera back TO before exiting freecam. ToggleFreeCameraMode's
		// off-toggle restores the engine's notion of the "prior camera", but that doesn't match OSF's
		// scene third-person HOLD (a bounce we drive in Tick, not a native camera state) — so without an
		// explicit handback the player is stranded in a loose camera after MMB exits. We resolve the
		// target here (game-thread state read of holdArmed) and apply it in the SAME task, AFTER the
		// engine toggle-off, so the ordering is deterministic and Tick's bounce can't race us.
		const bool handBackToHold = holdArmed.load(std::memory_order_relaxed);
		SFSE::GetTaskInterface()->AddTask([this, handBackToHold]() {
			if (!nativeFreeCamActive.exchange(false, std::memory_order_relaxed)) {
				return;  // not on — nothing to toggle
			}
			PopFreeCamInputContext();  // release the free-cam input context (reverse of enter)
			NativeToggleFreeCam();     // toggles off; the engine restores ITS prior camera (not OSF's hold)
			REX::INFO("Native free camera exited");

			// Now restore OSF's own posture. A re-acquired override means another holder owns the camera —
			// leave it. Otherwise hand back to the scene's third-person hold, or restore the saved baseline.
			if (suppressBounce.load(std::memory_order_relaxed)) {
				return;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			if (handBackToHold) {
				if (!camera->IsInThirdPerson()) {
					camera->ForceThirdPerson();
					REX::INFO("Player camera handed back to third-person hold after native free cam exit");
				}
				return;
			}
			RestoreBaseline();  // no scene hold active — restore the POV captured before the override
		});
	}

	void CameraService::DriveSceneOrbit()
	{
		std::unique_lock l{ driveLock, std::try_to_lock };
		if (!l.owns_lock()) {
			return;  // another job-thread Tick is already driving this frame
		}
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->QCameraEquals(RE::CameraState::kFreeFly)) {
			return;  // engine moved us out of the free-fly state — don't fight it
		}
		void* state = camera->cameraStates[RE::CameraState::kFreeFly];
		if (!state) {
			return;
		}

		// Stop the engine's free-fly Update from moving the position we own. Disassembly of the integrate
		// (FreeFlyCameraState::Update → 0x140fa6a00) shows the per-frame movement = SPEED(state+0xac) · dt ·
		// direction, where SPEED is read and scaled BEFORE the +0x2dd gate picks the direction source (global
		// vs the +0x9c.. accumulators). So zeroing only the accumulators couldn't help (the gate was clear →
		// direction came from the live global, and the speed scalar was still non-zero → ~0.2 m/frame snap).
		// Zeroing SPEED makes the movement zero in BOTH gate paths; speed is a persistent state field (not a
		// per-frame INI read — see camera-orbit memory), so writing it here holds it at 0. Velocity (+0x68/+0x6c)
		// and the accumulators (+0x9c..+0xa8) are zeroed too so nothing can integrate residual motion.
		{
			auto* sb = reinterpret_cast<std::byte*>(state);
			*reinterpret_cast<float*>(sb + 0xac) = 0.0f;  // free-fly translation SPEED — the movement scalar
			*reinterpret_cast<float*>(sb + 0x68) = 0.0f;  // velocity x
			*reinterpret_cast<float*>(sb + 0x6c) = 0.0f;  // velocity y
			*reinterpret_cast<float*>(sb + 0x9c) = 0.0f;  // WASD/look accumulators
			*reinterpret_cast<float*>(sb + 0xa0) = 0.0f;
			*reinterpret_cast<float*>(sb + 0xa4) = 0.0f;
			*reinterpret_cast<float*>(sb + 0xa8) = 0.0f;
		}

		const std::int64_t now = NowMs();
		float dt = (lastDriveMs == 0) ? 0.0f : static_cast<float>(now - lastDriveMs) / 1000.0f;
		lastDriveMs = now;
		dt = std::clamp(dt, 0.0f, 0.1f);

		// Control scheme (freecam-style orbit): MOUSE steers the orbit angle, WHEEL zooms the radius, and
		// WASD flies the orbit CENTER through the scene (the camera keeps orbiting the moving pivot).
		float mdx = 0.0f, mdy = 0.0f, wheel = 0.0f;
		auto& input = Input::InputService::GetSingleton();
		input.DrainMouseDelta(mdx, mdy);
		input.DrainWheelDelta(wheel);
		orbitTargetAzimuth -= mdx * kOrbitMouseSens;
		orbitTargetElevation += mdy * kOrbitMouseSens;
		orbitTargetElevation = std::clamp(orbitTargetElevation, -kOrbitElevLimit, kOrbitElevLimit);
		orbitTargetRadius -= wheel * kOrbitWheelStep;  // wheel up (+) = zoom in = smaller radius
		orbitTargetRadius = std::clamp(orbitTargetRadius, kOrbitMinRadius, kOrbitMaxRadius);

		// WASD pans the center in its horizontal plane, relative to the current view: W/S along the camera's
		// horizontal forward (toward/away from where it looks), A/D strafe. The center's height is unchanged.
		const float ch = std::cos(orbitAzimuth), sh = std::sin(orbitAzimuth);
		float panF = 0.0f, panR = 0.0f;
		if (KeyDown('W')) panF += 1.0f;
		if (KeyDown('S')) panF -= 1.0f;
		if (KeyDown('D')) panR += 1.0f;
		if (KeyDown('A')) panR -= 1.0f;
		if (panF != 0.0f || panR != 0.0f) {
			const float step = kOrbitPanSpeed * dt;
			orbitCenterX += (-sh * panF + ch * panR) * step;  // forward=(-sin,cos), right=(cos,sin)
			orbitCenterY += (ch * panF + sh * panR) * step;
		}

		// Low-pass the rendered values toward the targets (frame-rate-independent: alpha = 1 - e^(-dt/tau)).
		// This is what turns raw per-tick input into a smooth glide and absorbs the ~7×/frame tick cadence.
		const float alpha = (dt > 0.0f) ? (1.0f - std::exp(-dt / kOrbitSmoothTime)) : 1.0f;
		orbitAzimuth += (orbitTargetAzimuth - orbitAzimuth) * alpha;
		orbitElevation += (orbitTargetElevation - orbitElevation) * alpha;
		orbitRadius += (orbitTargetRadius - orbitRadius) * alpha;

		// Orbit the center (seeded on enter, then flown by WASD above). Camera sits radius behind it.
		const RE::NiPoint3 center{ orbitCenterX, orbitCenterY, orbitCenterZ };
		const RE::NiPoint3 fwd = LookDirection(orbitAzimuth, -orbitElevation);
		RE::NiPoint3       pos{
			center.x - fwd.x * orbitRadius,
			center.y - fwd.y * orbitRadius,
			center.z - fwd.z * orbitRadius
		};
		// Don't orbit through the floor: keep the camera above it (floor ≈ center minus the torso offset).
		const float floorZ = center.z - kOrbitCenterUp + kOrbitFloorMargin;
		pos.z = std::max(pos.z, floorZ);

		// Aim at the center from the (possibly floor-clamped) position so the look stays correct.
		const float dx = center.x - pos.x;
		const float dy = center.y - pos.y;
		const float dz = center.z - pos.z;
		const float yaw = std::atan2(-dx, dy);
		const float pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy));
		WriteFreeFlyTransform(state, pos, yaw, pitch);
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
		if (orbitDriving.exchange(false, std::memory_order_relaxed)) {
			Input::InputService::GetSingleton().SetMouseCapture(false);  // stop driving + reading mouse
		}
		if (nativeFreeCamActive.load(std::memory_order_relaxed)) {
			// NativeFreeCamExit toggles the engine free cam off AND performs OSF's own handback
			// (third-person hold, or baseline restore) in-order on the game thread — see there.
			NativeFreeCamExit();
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

	void CameraService::ToggleFreeCam()
	{
		// Re-entrant-safe alternation: the flag flips exactly once per press, so the on/off paths can't
		// double-acquire or double-release the override even if presses land back-to-back.
		if (!playerFreeCamHeld.exchange(true, std::memory_order_relaxed)) {
			AcquireStateOverride();                 // capture baseline + suppress the bounce on the first holder
			SetLiveCameraState(CameraMode::kFreeFly);  // engine-native free cam (the `tfc` path)
			REX::INFO("Player free camera toggled ON (MMB)");
			return;
		}
		playerFreeCamHeld.store(false, std::memory_order_relaxed);
		ReleaseStateOverride();  // hands the camera back to any scene posture / third-person hold / baseline
		REX::INFO("Player free camera toggled OFF (MMB)");
	}

	void CameraService::ForcePlayerFreeCamOff()
	{
		// Same release the MMB OFF-toggle does, but driven by scene teardown instead of a keypress: only
		// acts if the player toggle currently owns a hold (exchange guards against double-release). Without
		// it, a free cam toggled on mid-scene would survive SCENE_END (its override is not ledger-tracked),
		// stranding the player in free-fly. Ordering note: the scene runtime calls this from the input-grant
		// undo, which runs BEFORE the control-lock undo, so the third-person hold is still armed here and
		// ReleaseStateOverride hands the camera cleanly back to it (then the lock undo restores the baseline).
		if (playerFreeCamHeld.exchange(false, std::memory_order_relaxed)) {
			ReleaseStateOverride();
			REX::INFO("Player free camera force-exited (scene ended)");
		}
	}

	void CameraService::OnStopAll()
	{
		if (orbitDriving.exchange(false, std::memory_order_relaxed)) {
			Input::InputService::GetSingleton().SetMouseCapture(false);
		}
		if (nativeFreeCamActive.exchange(false, std::memory_order_relaxed)) {
			// Toggle the native free cam off so a save/load doesn't strand the player in it.
			SFSE::GetTaskInterface()->AddTask([]() { PopFreeCamInputContext(); NativeToggleFreeCam(); });
		}
		playerFreeCamHeld.store(false, std::memory_order_relaxed);
		std::scoped_lock l{ lock };
		holdCount = 0;
		overrideCount = 0;
		baselineCaptured = false;
		holdArmed.store(false, std::memory_order_relaxed);
		suppressBounce.store(false, std::memory_order_relaxed);
	}

	void CameraService::Tick()
	{
		if (orbitDriving.load(std::memory_order_relaxed)) {
			DriveSceneOrbit();  // self-driven scene orbit (independent of the third-person hold)
		}
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
