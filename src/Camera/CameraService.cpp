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

		// Orbit framing + control tunables (calibration expected on first run).
		constexpr float kOrbitCenterFwd = 0.5f;      // meters ahead of the player toward the partner (~pair midpoint)
		constexpr float kOrbitCenterUp = 1.0f;       // meters above the player's feet (torso height)
		constexpr float kOrbitFloorMargin = 0.3f;    // keep the camera at least this far above the floor
		constexpr float kOrbitDefaultRadius = 3.5f;  // meters
		constexpr float kOrbitDefaultElev = 0.32f;   // radians above the center (~18°)
		constexpr float kOrbitMinRadius = 1.0f;
		constexpr float kOrbitMaxRadius = 12.0f;
		constexpr float kOrbitMouseSens = 0.004f;    // radians per raw mouse unit
		constexpr float kOrbitZoomSpeed = 4.0f;      // meters/sec on W/S
		constexpr float kOrbitElevLimit = 1.45f;     // ~83°, off the gimbal poles

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
		SFSE::GetTaskInterface()->AddTask([this]() {
			if (!nativeFreeCamActive.exchange(false, std::memory_order_relaxed)) {
				return;  // not on — nothing to toggle
			}
			PopFreeCamInputContext();  // release the free-cam input context (reverse of enter)
			NativeToggleFreeCam();     // toggles off; the engine restores the prior camera
			REX::INFO("Native free camera exited");
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

		const std::int64_t now = NowMs();
		float dt = (lastDriveMs == 0) ? 0.0f : static_cast<float>(now - lastDriveMs) / 1000.0f;
		lastDriveMs = now;
		dt = std::clamp(dt, 0.0f, 0.1f);

		// Mouse steers the orbit; no auto-motion — it holds still when the mouse is idle.
		float mdx = 0.0f, mdy = 0.0f;
		Input::InputService::GetSingleton().DrainMouseDelta(mdx, mdy);
		orbitAzimuth -= mdx * kOrbitMouseSens;
		orbitElevation += mdy * kOrbitMouseSens;
		orbitElevation = std::clamp(orbitElevation, -kOrbitElevLimit, kOrbitElevLimit);

		// W/S zoom in/out.
		if (KeyDown('W')) {
			orbitRadius -= kOrbitZoomSpeed * dt;
		}
		if (KeyDown('S')) {
			orbitRadius += kOrbitZoomSpeed * dt;
		}
		orbitRadius = std::clamp(orbitRadius, kOrbitMinRadius, kOrbitMaxRadius);

		// Orbit the FIXED center captured on enter (stable → no jitter). Camera sits radius behind it.
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
			{
				std::scoped_lock l{ lock };
				baselineCaptured = false;  // ToggleFreeCameraMode restores the camera itself; drop our baseline
			}
			NativeFreeCamExit();
			return;  // engine owns the restore — skip our baseline/handback
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
		if (orbitDriving.exchange(false, std::memory_order_relaxed)) {
			Input::InputService::GetSingleton().SetMouseCapture(false);
		}
		if (nativeFreeCamActive.exchange(false, std::memory_order_relaxed)) {
			// Toggle the native free cam off so a save/load doesn't strand the player in it.
			SFSE::GetTaskInterface()->AddTask([]() { PopFreeCamInputContext(); NativeToggleFreeCam(); });
		}
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
