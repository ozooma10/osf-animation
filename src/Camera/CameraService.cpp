#include "Util/NativeMainThreadQueue.h"
#include "Camera/CameraService.h"

#include "Input/InputService.h"
#include "Player/PlayerInputLockService.h"
#include "RE/P/ThirdPersonState.h"

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
		constexpr std::uint32_t kNativeFreeCamState = 13;  // RE::CameraState::kFreeWalk
		std::atomic<bool> g_rawCameraLayoutsAvailable{ false };

		// PlayerCamera free-cam flags: +0x2dd = authoritative active gate (the console handler reads it), +0x2e5 = entered.
		constexpr std::ptrdiff_t kFreeCamGateOffset = 0x2dd;
		constexpr std::ptrdiff_t kFreeCamEnteredOffset = 0x2e5;

		std::uint8_t* FreeCamFlag(std::ptrdiff_t a_off)
		{
			if (!g_rawCameraLayoutsAvailable.load(std::memory_order_acquire)) {
				return nullptr;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			return camera ? reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::byte*>(camera) + a_off) : nullptr;
		}

		// Exit clears the flags only, never ToggleFreeCameraMode's exit; the caller's handback restores the POV.
		void SetNativeFreeCam(bool a_on)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			auto* gate = FreeCamFlag(kFreeCamGateOffset);
			auto* entered = FreeCamFlag(kFreeCamEnteredOffset);
			if (!gate || !entered) {
				return;
			}
			if (a_on) {
				if (*entered == 0) {
					camera->ToggleFreeCameraMode(kNativeFreeCamState, false);
				}
				return;
			}
			*gate = 0;
			*entered = 0;
		}

		// FreeFly state: position NiPoint3 world meters @+0x70, NiQuaternion (w,x,y,z) @+0x7c.
		constexpr std::ptrdiff_t kFreeFlyPosOffset = 0x70;
		constexpr std::ptrdiff_t kFreeFlyRotOffset = 0x7c;

		// Normalized zoom [0..2], not meters; <= 0 trips ForceFirstPerson.
		constexpr float kThirdPersonZoomMin = 0.1f;
		constexpr float kThirdPersonZoomMax = 2.0f;

		constexpr float kOrbitCenterFwd = 0.5f;      // meters toward the partner
		constexpr float kOrbitCenterUp = 1.0f;       // meters (torso height)
		constexpr float kOrbitFloorMargin = 0.3f;    // meters
		constexpr float kOrbitDefaultRadius = 3.5f;  // meters
		constexpr float kOrbitDefaultElev = 0.32f;   // radians (~18°)
		constexpr float kOrbitMinRadius = 1.0f;
		constexpr float kOrbitMaxRadius = 12.0f;
		constexpr float kOrbitMouseSens = 0.004f;    // radians per raw mouse unit
		constexpr float kOrbitPanSpeed = 5.0f;       // m/s
		constexpr float kOrbitLiftSpeed = 3.0f;      // m/s
		constexpr float kOrbitWheelStep = 0.6f;      // meters per wheel notch
		constexpr float kOrbitStickLookSpeed = 2.2f;    // rad/s at full deflection
		constexpr float kOrbitButtonZoomSpeed = 4.0f;   // m/s
		constexpr float kOrbitElevLimit = 1.45f;     // ~83°, off the gimbal poles
		constexpr float kOrbitSmoothTime = 0.07f;    // low-pass tau (s)
		constexpr float kOrbitReframeTime = 0.35f;   // tau during a reframe glide
		constexpr float kOrbitFrameFit = 2.5f;       // radius per meter of cast spread
		constexpr float kOrbitAxisMinSq = 0.04f;     // < 20 cm = co-located
		constexpr float kPi = 3.14159265f;

		float AngularGap(float a_a, float a_b)
		{
			return std::abs(std::remainder(a_a - a_b, 2.0f * kPi));
		}

		bool KeyDown(int a_vk)
		{
			return (GetAsyncKeyState(a_vk) & 0x8000) != 0;
		}

		bool InvertLookY()
		{
			// Cache the object, not the value: the Controls menu updates it in place.
			static const RE::Setting* setting = RE::GetINISetting("bInvertYValues:Controls");
			return setting && setting->GetType() == RE::Setting::Type::kBool && setting->GetBool();
		}

		// Game thread only. A pilot's camera lives in the ship state machine; Force*Person strands it.
		bool PlayerIsPilot()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			auto* ship = player->GetSpaceship();
			return ship && ship->GetSpaceshipPilot() == player;
		}

		std::int64_t NowMs()
		{
			using namespace std::chrono;
			return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
		}

		// forward = (-sin yaw, cos yaw), as Scene::PlacementToWorld; +pitch tilts up.
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
			if (!g_rawCameraLayoutsAvailable.load(std::memory_order_acquire)) {
				return;
			}
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

	void CameraService::SetRawLayoutSupport(bool a_enabled)
	{
		g_rawCameraLayoutsAvailable.store(a_enabled, std::memory_order_release);
	}

	bool CameraService::RawLayoutSupport() const
	{
		return g_rawCameraLayoutsAvailable.load(std::memory_order_acquire);
	}

	void CameraService::QueueTask(std::function<void()> a_task)
	{
		const auto epoch = taskEpoch.load(std::memory_order_acquire);
		Util::NativeMainThreadQueue::Post([this, epoch, task = std::move(a_task)]() mutable {
			if (taskEpoch.load(std::memory_order_acquire) == epoch) {
				task();
			}
		});
	}

	void CameraService::CaptureBaseline(RE::PlayerCamera& a_camera)
	{
		if (baselineCaptured) return;
		baselineWasFirstPerson = a_camera.IsInFirstPerson();
		baselineStateIndex = 0xFFFFFFFFu;
		for (std::uint32_t i = 0; i < RE::CameraState::kTotal; i++) {
			if (a_camera.QCameraEquals(static_cast<RE::CameraState>(i))) {
				baselineStateIndex = i;
				break;
			}
		}
		baselineCaptured = true;
	}

	void CameraService::RestoreBaseline()
	{
		QueueTask([this]() {
			bool          wantFirst = false;
			std::uint32_t priorState = 0xFFFFFFFFu;
			{
				std::scoped_lock l{ lock };
				// Another imposition still holds the camera; keep the baseline for it.
				if (holdCount > 0 || overrideCount > 0 || !baselineCaptured) {
					return;
				}
				wantFirst = baselineWasFirstPerson;
				priorState = baselineStateIndex;
				baselineCaptured = false;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			if (wantFirst) {
				if (!camera->IsInFirstPerson()) {
					if (PlayerIsPilot()) {
						// ForceFirstPerson bails for a seated pilot; set the cockpit state directly.
						camera->SetCameraState(RE::CameraState::kFirstPerson);
						REX::DEBUG("[Camera] restored cockpit first person after scene camera release (pilot)");
					} else {
						camera->ForceFirstPerson();
						REX::DEBUG("[Camera] restored to first person after scene camera release");
					}
				}
				return;
			}
			// A scene can capture bleedout before the player recovers; check life state at release.
			if (priorState == RE::CameraState::kBleedout) {
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player) return;
				// ActorState documents the life-state field in actorState1 bits 17-20.
				const auto lifeState = static_cast<RE::ACTOR_LIFE_STATE>((player->actorState1 >> 17) & 0xF);
				if (lifeState != RE::ACTOR_LIFE_STATE::kBleedOut && lifeState != RE::ACTOR_LIFE_STATE::kEssentialDown) {
					priorState = RE::CameraState::kThirdPerson;
					REX::DEBUG("[Camera] discarded stale bleedout camera after scene camera release (life state {})", static_cast<std::uint32_t>(lifeState));
				}
			}
			// Hand engine-owned states (ship exterior, vehicle, furniture) back exactly; forcing on-foot third person strands the camera.
			const bool engineAltState = priorState < RE::CameraState::kTotal &&
			                            priorState != RE::CameraState::kFirstPerson &&
			                            priorState != RE::CameraState::kThirdPerson &&
			                            priorState != RE::CameraState::kFreeFly &&
			                            priorState != RE::CameraState::kFreeWalk &&
			                            priorState != RE::CameraState::kAutoVanity;
			if (engineAltState) {
				if (!camera->QCameraEquals(static_cast<RE::CameraState>(priorState))) {
					camera->SetCameraState(static_cast<RE::CameraState>(priorState));
					REX::DEBUG("[Camera] restored prior camera state {} after scene camera release", priorState);
				}
			} else if (!camera->IsInThirdPerson()) {
				camera->ForceThirdPerson();
				REX::DEBUG("[Camera] restored to third person after scene camera release");
			}
		});
	}

	void CameraService::SetStandaloneLock(bool a_enable)
	{
		if (a_enable) {
			{
				std::scoped_lock l{ lock };
				if (++holdCount != 1) {
					return;
				}
			}
			QueueTask([this]() {
				auto* camera = RE::PlayerCamera::GetSingleton();
				if (!camera) return;
				{
					std::scoped_lock l{ lock };
					if (holdCount == 0) return;
					CaptureBaseline(*camera);
					holdArmed.store(true, std::memory_order_relaxed);
				}
				if (suppressBounce.load(std::memory_order_relaxed)) {
					return;
				}
				if (!camera->IsInThirdPerson()) {
					camera->ForceThirdPerson();
					REX::DEBUG("[Camera] forced to third person for standalone lock");
				}
			});
		} else {
			{
				std::scoped_lock l{ lock };
				if (holdCount == 0) {
					return;
				}
				if (--holdCount != 0) {
					return;
				}
				holdArmed.store(false, std::memory_order_relaxed);
			}
			RestoreBaseline();
		}
	}

	void CameraService::KickToThirdPerson()
	{
		QueueTask([this]() {
			if (suppressBounce.load(std::memory_order_relaxed)) {
				return;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !camera->IsInFirstPerson()) {
				return;
			}
			// A pilot's first person is the cockpit; on-foot third person strands the camera.
			if (PlayerIsPilot()) {
				REX::DEBUG("[Camera] first-person kick skipped — player is piloting a ship");
				return;
			}
			camera->ForceThirdPerson();
			// ForceThirdPerson resets zoom to 0, which re-trips ForceFirstPerson; snap the pull-back.
			SeedThirdPersonZoom(0.0f, /*snapCurrent*/ true);
			REX::DEBUG("[Camera] kicked out of first person (external request)");
		});
	}

	void CameraService::SeedThirdPersonZoom(float a_distance, bool a_snapCurrent)
	{
		if (a_distance < 0.0f) {
			return;  // < 0 = don't seed
		}
		// 0 = fully out; positive = authored distance.
		const float seed = (a_distance > 0.0f)
			? std::clamp(a_distance, kThirdPersonZoomMin, kThirdPersonZoomMax)
			: kThirdPersonZoomMax;
		QueueTask([this, seed, a_snapCurrent]() {
			if (suppressBounce.load(std::memory_order_relaxed)) {
				return;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			// Write the state directly; a tween frame mid-ForceThirdPerson must not skip the seed.
			auto* thirdPerson = static_cast<RE::ThirdPersonState*>(camera->cameraStates[RE::CameraState::kThirdPerson]);
			if (!thirdPerson) {
				return;
			}
			thirdPerson->targetZoomOffset = seed;
			if (a_snapCurrent) {
				thirdPerson->currentZoomOffset = seed;
			}
			REX::TRACE("[Camera] third-person start zoom seeded to {} (snapCurrent={})", seed, a_snapCurrent);
		});
	}

	void CameraService::AcquireStateOverride()
	{
		{
			std::scoped_lock l{ lock };
			if (++overrideCount != 1) {
				return;
			}
			suppressBounce.store(true, std::memory_order_relaxed);
		}
		QueueTask([this]() {
			if (auto* camera = RE::PlayerCamera::GetSingleton()) {
				std::scoped_lock l{ lock };
				if (overrideCount > 0) CaptureBaseline(*camera);
			}
		});
	}

	void CameraService::SetLiveCameraState(CameraMode a_mode)
	{
		if ((a_mode == CameraMode::kFreeFly || a_mode == CameraMode::kSceneOrbit) && !RawLayoutSupport()) {
			static std::atomic<bool> warned{ false };
			if (!warned.exchange(true, std::memory_order_relaxed)) {
				REX::WARN("[Camera] free-fly/orbit disabled: raw camera layouts are not verified for this game version; using vanity orbit");
			}
			a_mode = CameraMode::kVanityOrbit;
		}
		if (a_mode == CameraMode::kFreeFly) {
			NativeFreeCamEnter(/*a_gamepadPassthrough*/ true);  // engine owns camera + input
			return;
		}
		if (a_mode == CameraMode::kSceneOrbit) {
			// Bootstrap through native TFC to keep its close-actor rendering policy; FIFO tasks set the flags before the FreeFly handoff.
			NativeFreeCamEnter(/*a_gamepadPassthrough*/ false);
			QueueTask([this]() {
				if (!suppressBounce.load(std::memory_order_relaxed)) {
					return;
				}
				auto* camera = RE::PlayerCamera::GetSingleton();
				if (!camera) {
					return;
				}
				// Leave first person first, or FreeFly keeps the 1st-person skin (floating hands).
				if (camera->IsInFirstPerson()) {
					camera->ForceThirdPerson();
				}
				camera->SetCameraState(RE::CameraState::kFreeFly);
				auto* player = RE::PlayerCharacter::GetSingleton();
				{
					std::scoped_lock l{ driveLock };
					const float        h = player ? player->data.angle.z : 0.0f;
					const RE::NiPoint3 loc = player ? player->data.location : RE::NiPoint3{};
					// Cast seed: center on the midpoint, fit the spread, open side-on to the long axis (side nearest the player's heading).
					std::vector<RE::NiPoint3> pts;
					pts.reserve(frameSubjects.size());
					for (const std::uint32_t id : frameSubjects) {
						if (const auto* subject = RE::TESForm::LookupByID<RE::Actor>(id)) {
							pts.push_back(subject->data.location);
						}
					}
					frameSubjects.clear();
					float azimuth = h;
					float radius = kOrbitDefaultRadius;
					float centerX, centerY, centerZ;
					if (pts.empty()) {
						centerX = loc.x - std::sin(h) * kOrbitCenterFwd;
						centerY = loc.y + std::cos(h) * kOrbitCenterFwd;
						centerZ = loc.z + kOrbitCenterUp;
					} else {
						float cx = 0.0f, cy = 0.0f, cz = 0.0f;
						for (const auto& p : pts) {
							cx += p.x;
							cy += p.y;
							cz += p.z;
						}
						const float n = static_cast<float>(pts.size());
						cx /= n;
						cy /= n;
						cz /= n;
						centerX = cx;
						centerY = cy;
						centerZ = cz + kOrbitCenterUp;
						float spread = 0.0f;
						float axX = 0.0f, axY = 0.0f, axLenSq = 0.0f;
						for (std::size_t i = 0; i < pts.size(); i++) {
							spread = std::max(spread, std::hypot(pts[i].x - cx, pts[i].y - cy));
							for (std::size_t j = i + 1; j < pts.size(); j++) {
								const float dx = pts[j].x - pts[i].x;
								const float dy = pts[j].y - pts[i].y;
								const float d2 = dx * dx + dy * dy;
								if (d2 > axLenSq) {
									axLenSq = d2;
									axX = dx;
									axY = dy;
								}
							}
						}
						radius = std::clamp(std::max(kOrbitDefaultRadius, spread * kOrbitFrameFit), kOrbitMinRadius, kOrbitMaxRadius);
						if (axLenSq > kOrbitAxisMinSq) {
							const float axisYaw = std::atan2(-axX, axY);
							const float side1 = axisYaw + 0.5f * kPi;
							const float side2 = axisYaw - 0.5f * kPi;
							azimuth = (AngularGap(side1, h) <= AngularGap(side2, h)) ? side1 : side2;
						}
					}
					if (orbitDriving.load(std::memory_order_relaxed)) {
						// Live orbit: the user may have steered it, so glide from the rendered pose; lastDriveMs stays live.
					} else {
						// Fresh enter: start near the old third-person view so the framing arrives as a swoop.
						orbitAzimuth = h;
						orbitElevation = kOrbitDefaultElev;
						orbitRadius = kOrbitDefaultRadius;
						orbitCenterX = loc.x;
						orbitCenterY = loc.y;
						orbitCenterZ = loc.z + kOrbitCenterUp;
						lastDriveMs = 0;
					}
					// Unwrap to the nearest turn; a dragged orbit accumulates whole revolutions.
					orbitTargetAzimuth = orbitAzimuth + std::remainder(azimuth - orbitAzimuth, 2.0f * kPi);
					orbitTargetElevation = kOrbitDefaultElev;
					orbitTargetRadius = radius;
					orbitCenterTargetX = centerX;
					orbitCenterTargetY = centerY;
					orbitCenterTargetZ = centerZ;
					orbitFloorZ = centerZ - kOrbitCenterUp + kOrbitFloorMargin;
					orbitReframeGlide = true;
					lastOrbitPoseValid = false;
				}
				orbitDriving.store(true, std::memory_order_relaxed);
				Input::InputService::GetSingleton().SetMouseCapture(true);
				REX::DEBUG("[Camera] native-assisted scene orbit engaged");
			});
			return;
		}
		// Stop any TFC driver before vanity; the exit is queued unconditionally to cover a pending enter task.
		if (orbitDriving.exchange(false, std::memory_order_relaxed)) {
			Input::InputService::GetSingleton().SetMouseCapture(false);
		}
		NativeFreeCamExit();
		QueueTask([this]() {
			if (!suppressBounce.load(std::memory_order_relaxed)) {
				return;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			if (camera->IsInFirstPerson()) {
				camera->ForceThirdPerson();
			}
			camera->SetCameraState(RE::CameraState::kAutoVanity);
			REX::DEBUG("[Camera] set to vanity orbit");
		});
	}

	void CameraService::SetOrbitFrameSubjects(std::vector<std::uint32_t> a_actorIds)
	{
		std::scoped_lock l{ driveLock };
		frameSubjects = std::move(a_actorIds);
	}

	void CameraService::NativeFreeCamEnter(bool a_gamepadPassthrough)
	{
		if (!RawLayoutSupport()) {
			return;
		}
		QueueTask([this, a_gamepadPassthrough]() {
			if (!suppressBounce.load(std::memory_order_relaxed)) {
				return;
			}
			Input::InputService::GetSingleton().SetNativeFreeCamGamepad(a_gamepadPassthrough);
			if (a_gamepadPassthrough && orbitDriving.exchange(false, std::memory_order_relaxed)) {
				Input::InputService::GetSingleton().SetMouseCapture(false);
			}
			if (nativeFreeCamActive.exchange(true, std::memory_order_relaxed)) {
				// orbit -> pure freefly must restore kFreeWalk; freefly -> orbit is finished by the caller's FreeFly task.
				if (a_gamepadPassthrough) {
					if (auto* camera = RE::PlayerCamera::GetSingleton()) {
						camera->SetCameraState(RE::CameraState::kFreeWalk);
					}
				}
				return;  // toggling again would exit
			}
			SetNativeFreeCam(true);
			REX::DEBUG("[Camera] native free camera entered (ToggleFreeCameraMode, state {}, input={})",
				kNativeFreeCamState, a_gamepadPassthrough ? "native" : "OSF orbit");
		});
	}

	void CameraService::NativeFreeCamExit()
	{
		// ToggleFreeCameraMode's exit doesn't know OSF's third-person hold; resolve the handback now, apply it after the flag clear.
		const bool handBackToHold = holdArmed.load(std::memory_order_relaxed);
		QueueTask([this, handBackToHold]() {
			if (!nativeFreeCamActive.exchange(false, std::memory_order_relaxed)) {
				return;
			}
			Input::InputService::GetSingleton().SetNativeFreeCamGamepad(false);
			SetNativeFreeCam(false);
			// tfc sets the player AI-driven; toggling off doesn't reliably clear it for a pinned participant.
			Player::PlayerInputLockService::GetSingleton().ClearAIDriven();

			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!suppressBounce.load(std::memory_order_relaxed) && camera) {
				if (handBackToHold) {
					if (!camera->IsInThirdPerson()) {
						camera->ForceThirdPerson();
						REX::DEBUG("[Camera] handed back to third-person hold after native free cam exit");
					}
					SeedThirdPersonZoom(0.0f, /*snapCurrent*/ true);
				} else {
					RestoreBaseline();
				}
			}

			REX::DEBUG("[Camera] native free camera exited");
		});
	}

	void CameraService::RestoreAfterPlayerFreeCam(PlayerFreeCamReturn a_mode)
	{
		if (a_mode == PlayerFreeCamReturn::kNone) {
			return;
		}
		if (a_mode == PlayerFreeCamReturn::kSceneOrbit) {
			// Keep TFC's renderer flags, return the gamepad to OSF, resume the retained orbit in kFreeFly.
			QueueTask([this]() {
				if (!suppressBounce.load(std::memory_order_relaxed) ||
				    !nativeFreeCamActive.load(std::memory_order_relaxed) ||
				    !orbitDriving.load(std::memory_order_relaxed)) {
					return;
				}
				Input::InputService::GetSingleton().SetNativeFreeCamGamepad(false);
				if (auto* camera = RE::PlayerCamera::GetSingleton()) {
					camera->SetCameraState(RE::CameraState::kFreeFly);
					Input::InputService::GetSingleton().SetMouseCapture(true);
					REX::DEBUG("[Camera] native free camera handed back to the prior native-assisted scene orbit");
				}
			});
			return;
		}
		// The scene still holds vanity, so releasing only the ref-count would leave TFC live; exit it explicitly.
		NativeFreeCamExit();
		QueueTask([this, a_mode]() {
			if (!suppressBounce.load(std::memory_order_relaxed)) {
				return;
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			if (a_mode == PlayerFreeCamReturn::kVanityOrbit) {
				camera->SetCameraState(RE::CameraState::kAutoVanity);
				REX::DEBUG("[Camera] native free camera handed back to the prior vanity orbit");
			}
		});
	}

	void CameraService::DriveSceneOrbit()
	{
		if (!RawLayoutSupport()) {
			orbitDriving.store(false, std::memory_order_relaxed);
			return;
		}
		std::unique_lock l{ driveLock, std::try_to_lock };
		if (!l.owns_lock()) {
			return;
		}
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->QCameraEquals(RE::CameraState::kFreeFly)) {
			return;
		}
		void* state = camera->cameraStates[RE::CameraState::kFreeFly];
		if (!state) {
			return;
		}

		// FreeFlyCameraState::Update moves by SPEED(+0xac) * dt * dir in both +0x2dd gate paths. Zero SPEED (persistent),
		// velocity (+0x68/+0x6c) and accumulators (+0x9c..+0xa8) so the engine can't move our position.
		{
			auto* sb = reinterpret_cast<std::byte*>(state);
			*reinterpret_cast<float*>(sb + 0xac) = 0.0f;
			*reinterpret_cast<float*>(sb + 0x68) = 0.0f;
			*reinterpret_cast<float*>(sb + 0x6c) = 0.0f;
			*reinterpret_cast<float*>(sb + 0x9c) = 0.0f;
			*reinterpret_cast<float*>(sb + 0xa0) = 0.0f;
			*reinterpret_cast<float*>(sb + 0xa4) = 0.0f;
			*reinterpret_cast<float*>(sb + 0xa8) = 0.0f;
		}

		const std::int64_t now = NowMs();
		float dt = (lastDriveMs == 0) ? 0.0f : static_cast<float>(now - lastDriveMs) / 1000.0f;
		lastDriveMs = now;
		dt = std::clamp(dt, 0.0f, 0.1f);

		float mdx = 0.0f, mdy = 0.0f, wheel = 0.0f;
		auto& input = Input::InputService::GetSingleton();
		input.DrainMouseDelta(mdx, mdy);
		input.DrainWheelDelta(wheel);
		const auto pad = input.ReadGamepadState();
		const float padLift = pad.rightTrigger - pad.leftTrigger;
		const float padZoom = (pad.rightShoulder ? 1.0f : 0.0f) - (pad.leftShoulder ? 1.0f : 0.0f);
		const bool padSteering = pad.rightX != 0.0f || pad.rightY != 0.0f || pad.leftX != 0.0f ||
			pad.leftY != 0.0f || padLift != 0.0f || padZoom != 0.0f;
		const bool keyW = KeyDown('W');
		const bool keyS = KeyDown('S');
		const bool keyD = KeyDown('D');
		const bool keyA = KeyDown('A');
		// Stick up = camera lowers, matching a mouse drag up (-mdy).
		orbitTargetAzimuth -= mdx * kOrbitMouseSens + pad.rightX * kOrbitStickLookSpeed * dt;
		const float lookY = mdy * kOrbitMouseSens - pad.rightY * kOrbitStickLookSpeed * dt;
		orbitTargetElevation += InvertLookY() ? -lookY : lookY;
		orbitTargetElevation = std::clamp(orbitTargetElevation, -kOrbitElevLimit, kOrbitElevLimit);
		orbitTargetRadius -= wheel * kOrbitWheelStep + padZoom * kOrbitButtonZoomSpeed * dt;
		orbitTargetRadius = std::clamp(orbitTargetRadius, kOrbitMinRadius, kOrbitMaxRadius);

		const float ch = std::cos(orbitAzimuth), sh = std::sin(orbitAzimuth);
		float panF = pad.leftY, panR = pad.leftX;
		if (keyW) panF += 1.0f;
		if (keyS) panF -= 1.0f;
		if (keyD) panR += 1.0f;
		if (keyA) panR -= 1.0f;
		panF = std::clamp(panF, -1.0f, 1.0f);
		panR = std::clamp(panR, -1.0f, 1.0f);
		if (panF != 0.0f || panR != 0.0f) {
			const float step = kOrbitPanSpeed * dt;
			orbitCenterTargetX += (-sh * panF + ch * panR) * step;  // forward=(-sin,cos), right=(cos,sin)
			orbitCenterTargetY += (ch * panF + sh * panR) * step;
		}
		orbitCenterTargetZ = std::max(orbitCenterTargetZ + padLift * kOrbitLiftSpeed * dt, orbitFloorZ);

		if (mdx != 0.0f || mdy != 0.0f || wheel != 0.0f || panF != 0.0f || panR != 0.0f || padSteering) {
			orbitReframeGlide = false;
		}

		// Frame-rate-independent low-pass; alpha = 0 on the first frame (dt unknown).
		const float tau = orbitReframeGlide ? kOrbitReframeTime : kOrbitSmoothTime;
		const float alpha = (dt > 0.0f) ? (1.0f - std::exp(-dt / tau)) : 0.0f;
		orbitAzimuth += (orbitTargetAzimuth - orbitAzimuth) * alpha;
		orbitElevation += (orbitTargetElevation - orbitElevation) * alpha;
		orbitRadius += (orbitTargetRadius - orbitRadius) * alpha;
		orbitCenterX += (orbitCenterTargetX - orbitCenterX) * alpha;
		orbitCenterY += (orbitCenterTargetY - orbitCenterY) * alpha;
		orbitCenterZ += (orbitCenterTargetZ - orbitCenterZ) * alpha;
		if (orbitReframeGlide &&
			std::abs(orbitTargetAzimuth - orbitAzimuth) < 0.01f &&
			std::abs(orbitTargetElevation - orbitElevation) < 0.01f &&
			std::abs(orbitTargetRadius - orbitRadius) < 0.05f &&
			std::abs(orbitCenterTargetX - orbitCenterX) < 0.05f &&
			std::abs(orbitCenterTargetY - orbitCenterY) < 0.05f &&
			std::abs(orbitCenterTargetZ - orbitCenterZ) < 0.05f) {
			orbitReframeGlide = false;
		}

		const RE::NiPoint3 center{ orbitCenterX, orbitCenterY, orbitCenterZ };
		const RE::NiPoint3 fwd = LookDirection(orbitAzimuth, -orbitElevation);
		RE::NiPoint3       pos{
			center.x - fwd.x * orbitRadius,
			center.y - fwd.y * orbitRadius,
			center.z - fwd.z * orbitRadius
		};
		pos.z = std::max(pos.z, orbitFloorZ);

		const float dx = center.x - pos.x;
		const float dy = center.y - pos.y;
		const float dz = center.z - pos.z;
		const float yaw = std::atan2(-dx, dy);
		const float pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy));
		WriteFreeFlyTransform(state, pos, yaw, pitch);

		const float aimLen = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (aimLen > 0.001f) {
			lastOrbitPos[0] = pos.x;
			lastOrbitPos[1] = pos.y;
			lastOrbitPos[2] = pos.z;
			lastOrbitFwd[0] = dx / aimLen;
			lastOrbitFwd[1] = dy / aimLen;
			lastOrbitFwd[2] = dz / aimLen;
			lastOrbitPoseValid = true;
		}
	}

	bool CameraService::SceneOrbitPose(float (&a_pos)[3], float (&a_fwd)[3])
	{
		if (!orbitDriving.load(std::memory_order_relaxed)) {
			return false;
		}
		std::unique_lock l{ driveLock, std::try_to_lock };
		if (!l.owns_lock() || !lastOrbitPoseValid) {
			return false;
		}
		std::copy(std::begin(lastOrbitPos), std::end(lastOrbitPos), a_pos);
		std::copy(std::begin(lastOrbitFwd), std::end(lastOrbitFwd), a_fwd);
		return true;
	}

	void CameraService::ReleaseStateOverride()
	{
		bool released = false;
		bool inspectionHandback = false;
		{
			std::scoped_lock l{ lock };
			if (overrideCount == 0) {
				return;
			}
			if (--overrideCount != 0) {
				// Only the inspection hold remains after a scene released a non-orbit camera: hand back to the inspection orbit.
				inspectionHandback = overrideCount == 1 &&
				                     inspectionOrbitHolds.load(std::memory_order_relaxed) &&
				                     !orbitDriving.load(std::memory_order_relaxed) &&
				                     !nativeFreeCamActive.load(std::memory_order_relaxed);
			} else {
				released = true;
				suppressBounce.store(false, std::memory_order_relaxed);
			}
		}
		if (inspectionHandback) {
			SetLiveCameraState(CameraMode::kSceneOrbit);  // no seed -> centers on the player
			REX::DEBUG("[Camera] scene camera released while inspecting — handed back to the inspection orbit");
			return;
		}
		if (!released) {
			return;
		}
		if (orbitDriving.exchange(false, std::memory_order_relaxed)) {
			Input::InputService::GetSingleton().SetMouseCapture(false);
		}
		if (nativeFreeCamActive.load(std::memory_order_relaxed)) {
			NativeFreeCamExit();
			return;
		}
		if (holdArmed.load(std::memory_order_relaxed)) {
			QueueTask([this]() {
				// Both restores are queued; re-check the live hold so a stale snapshot doesn't preempt RestoreBaseline's POV re-entry.
				if (suppressBounce.load(std::memory_order_relaxed) || !holdArmed.load(std::memory_order_relaxed)) {
					return;
				}
				auto* camera = RE::PlayerCamera::GetSingleton();
				if (camera && !camera->IsInThirdPerson()) {
					camera->ForceThirdPerson();
					REX::DEBUG("[Camera] handed back to third-person hold after state override");
				}
			});
		} else {
			RestoreBaseline();
		}
	}

	void CameraService::ToggleFreeCam()
	{
		if (!RawLayoutSupport()) {
			REX::DEBUG("[Camera] player free camera unavailable on this game version");
			return;
		}
		// Flag flips once per press, so on/off can't double-acquire or double-release.
		if (!playerFreeCamHeld.exchange(true, std::memory_order_relaxed)) {
			// A scene-authored pure freefly stays scene-owned; only a native-assisted orbit swaps to pure TFC.
			const bool nativeActive = nativeFreeCamActive.load(std::memory_order_relaxed);
			const bool orbitActive = orbitDriving.load(std::memory_order_relaxed);
			if (nativeActive && !orbitActive) {
				playerFreeCamHeld.store(false, std::memory_order_relaxed);
				REX::DEBUG("[Camera] player free camera toggle ignored — scene free-fly already active");
				return;
			}
			PlayerFreeCamReturn returnMode = PlayerFreeCamReturn::kNone;
			// A node may have retargeted a retained orbit to vanity; prefer the live camera state.
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->QCameraEquals(RE::CameraState::kAutoVanity)) {
				returnMode = PlayerFreeCamReturn::kVanityOrbit;
			} else if (orbitDriving.load(std::memory_order_relaxed)) {
				returnMode = PlayerFreeCamReturn::kSceneOrbit;
			}
			playerFreeCamReturn.store(returnMode, std::memory_order_relaxed);
			AcquireStateOverride();
			if (returnMode == PlayerFreeCamReturn::kSceneOrbit && nativeActive) {
				// Orbit state is retained (Tick pauses outside kFreeFly); hand TFC back to native input.
				Input::InputService::GetSingleton().SetMouseCapture(false);
				Input::InputService::GetSingleton().SetNativeFreeCamGamepad(true);
				QueueTask([this]() {
					if (suppressBounce.load(std::memory_order_relaxed) &&
					    nativeFreeCamActive.load(std::memory_order_relaxed)) {
						if (auto* camera = RE::PlayerCamera::GetSingleton()) {
							camera->SetCameraState(RE::CameraState::kFreeWalk);
						}
					}
				});
			} else {
				SetLiveCameraState(CameraMode::kFreeFly);
			}
			REX::DEBUG("[Camera] player free camera toggled ON (MMB/R3)");
			return;
		}
		playerFreeCamHeld.store(false, std::memory_order_relaxed);
		RestoreAfterPlayerFreeCam(playerFreeCamReturn.exchange(PlayerFreeCamReturn::kNone, std::memory_order_relaxed));
		ReleaseStateOverride();
		REX::DEBUG("[Camera] player free camera toggled OFF (MMB/R3)");
	}

	void CameraService::ForcePlayerFreeCamOff()
	{
		// Runs from the input-grant undo, before the control-lock undo, so the hold is still armed for the handback.
		if (playerFreeCamHeld.exchange(false, std::memory_order_relaxed)) {
			RestoreAfterPlayerFreeCam(playerFreeCamReturn.exchange(PlayerFreeCamReturn::kNone, std::memory_order_relaxed));
			ReleaseStateOverride();
			REX::DEBUG("[Camera] player free camera force-exited (scene ended)");
		}
	}

	bool CameraService::SceneOrbitAvailable() const
	{
		if (!RawLayoutSupport()) {
			REX::DEBUG("[Camera] scene orbit unavailable — raw layouts are not verified for this game version");
			return false;
		}
		// The orbit writes an absolute world transform; in space the ship frame re-bases underneath it (violent spinning).
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			if (auto* ship = player->GetSpaceship()) {
				// Not IsSpaceshipLanded/Docked: placeholder-0 IDs in this CommonLibSF (crash on lookup).
				if (ship->IsInSpace(true)) {
					REX::DEBUG("[Camera] scene orbit unavailable — aboard a ship in space");
					return false;
				}
				REX::DEBUG("[Camera] scene orbit available — ship is landed");
			}
		}
		return true;
	}

	bool CameraService::AcquireInspectionOrbit(std::vector<std::uint32_t> a_frameSubjects)
	{
		// Re-check before the held flag so landing during inspection re-enables it.
		if (!SceneOrbitAvailable()) {
			return false;
		}

		if (inspectionOrbitHolds.fetch_add(1, std::memory_order_relaxed) != 0) {
			return true;
		}
		AcquireStateOverride();
		// A live scene camera keeps its framing; retargeting would yank it from the user.
		if (orbitDriving.load(std::memory_order_relaxed) || nativeFreeCamActive.load(std::memory_order_relaxed)) {
			REX::DEBUG("[Camera] inspection orbit hold taken (a scene camera is live — not retargeting)");
			return true;
		}
		SetOrbitFrameSubjects(std::move(a_frameSubjects));
		SetLiveCameraState(CameraMode::kSceneOrbit);
		REX::DEBUG("[Camera] inspection orbit engaged (external consumer)");
		return true;
	}

	void CameraService::ReleaseInspectionOrbit()
	{
		// Drop the final hold first so ReleaseStateOverride's handback cannot self-trigger.
		if (inspectionOrbitHolds.load(std::memory_order_relaxed) != 0 &&
			inspectionOrbitHolds.fetch_sub(1, std::memory_order_relaxed) == 1) {
			ReleaseStateOverride();
			REX::DEBUG("[Camera] inspection orbit released (external consumer)");
		}
	}

	void CameraService::OnStopAll()
	{
		taskEpoch.fetch_add(1, std::memory_order_acq_rel);
		Input::InputService::GetSingleton().SetNativeFreeCamGamepad(false);
		if (orbitDriving.exchange(false, std::memory_order_relaxed)) {
			Input::InputService::GetSingleton().SetMouseCapture(false);
		}
		{
			std::scoped_lock dl{ driveLock };
			frameSubjects.clear();  // a stale seed must not frame a post-load scene
		}
		if (nativeFreeCamActive.exchange(false, std::memory_order_relaxed)) {
			QueueTask([]() { SetNativeFreeCam(false); });
		}
		playerFreeCamHeld.store(false, std::memory_order_relaxed);
		playerFreeCamReturn.store(PlayerFreeCamReturn::kNone, std::memory_order_relaxed);
		inspectionOrbitHolds.store(0, std::memory_order_relaxed);
		std::scoped_lock l{ lock };
		holdCount = 0;
		overrideCount = 0;
		baselineCaptured = false;
		holdArmed.store(false, std::memory_order_relaxed);
		suppressBounce.store(false, std::memory_order_relaxed);
	}

	void CameraService::OnPostLoad()
	{
		QueueTask([]() {
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			const bool leaked = camera->QCameraEquals(RE::CameraState::kFreeFly) ||
			                    camera->QCameraEquals(RE::CameraState::kFreeWalk) ||
			                    camera->QCameraEquals(RE::CameraState::kAutoVanity);
			if (!leaked) {
				return;
			}
			SetNativeFreeCam(false);
			if (PlayerIsPilot()) {
				camera->SetCameraState(RE::CameraState::kFirstPerson);  // Force* strands a seated pilot
				REX::INFO("[Camera] recovered a leaked scene-camera state after load (restored cockpit)");
			} else {
				camera->ForceThirdPerson();
				REX::INFO("[Camera] recovered a leaked scene-camera state after load (forced third person)");
			}
		});
	}

	void CameraService::Tick()
	{
		if (orbitDriving.load(std::memory_order_relaxed)) {
			DriveSceneOrbit();
		}
		if (!holdArmed.load(std::memory_order_relaxed)) {
			return;
		}
		if (suppressBounce.load(std::memory_order_relaxed)) {
			return;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) {
			return;
		}

		if (!camera->IsInFirstPerson()) {
			return;
		}

		camera->ForceThirdPerson();
		REX::TRACE("[Camera] bounced back to third person mid-lock");
	}
}
