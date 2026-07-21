#include "Camera/CameraService.h"

#include "Input/InputService.h"
#include "Player/PlayerControlService.h"

#include <algorithm>
#include <chrono>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput9_1_0.lib")  // ships with Windows — no redistributable, works on every slot

namespace OSF::Camera
{
	namespace
	{
		constexpr std::uint32_t kNativeFreeCamState = 13;  // RE::CameraState::kFreeWalk (== plain `tfc`)
		using ToggleFreeCam_t = void (*)(RE::PlayerCamera*, std::uint32_t, bool);

		// The engine's two native free-cam flags on PlayerCamera
		//   +0x2dd "gate"    - the AUTHORITATIVE free-cam-active flag the console handler reads to pick enter vs exit.
		//   +0x2e5 "entered" — set on enter / cleared on exit; (WASD drives the cam, not the player), even after the view is third-person.
		constexpr std::ptrdiff_t kFreeCamGateOffset = 0x2dd;
		constexpr std::ptrdiff_t kFreeCamEnteredOffset = 0x2e5;

		std::uint8_t* FreeCamFlag(std::ptrdiff_t a_off)
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			return camera ? reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::byte*>(camera) + a_off) : nullptr;
		}

		// Drive the engine free cam to a desired on/off state.
		//   ENTER: call ToggleFreeCameraMode - the engine sets up the FreeWalk state + flags properly. (Safe.)
		//   EXIT:  DO NOT call ToggleFreeCameraMode's exit. The camera itself is returned to third person by the caller's ForceThirdPerson / RestoreBaseline handback
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
					camera->ToggleFreeCameraMode(kNativeFreeCamState, false);  // enter (gate==0 -> ENTER branch)
				}
				return;
			}
			*gate = 0;     // engine no longer considers free cam active...
			*entered = 0;  // ...nor entered -> input routes back to the current (about-to-be third-person) state.
		}

		// --- Scene-orbit self-drive. We place the camera on a ring around the scene center and aim it
		// inward, writing the FreeFly state's transform each frame (the engine renders from +0x70/+0x7c).
		// State @ PlayerCamera::cameraStates[kFreeFly]; pos = NiPoint3 world METERS @+0x70, orientation =
		// NiQuaternion (w,x,y,z) @+0x7c (OSF RE camera.state_machine, in-game proven).
		constexpr std::ptrdiff_t kFreeFlyPosOffset = 0x70;
		constexpr std::ptrdiff_t kFreeFlyRotOffset = 0x7c;

		// The engine auto-lerps current(+0x228) -> target(+0x224) each frame (~fMouseWheelZoomSpeed 3.0/s, ~1-2 s), so write the TARGET once and the camera glides; 
		// ForceThirdPerson resets zoom to 0.0 so seed a pull-back right after it.
		constexpr std::ptrdiff_t kThirdPersonTargetZoomOffset = 0x224;   // float — the zoom SETPOINT (write this)
		constexpr std::ptrdiff_t kThirdPersonCurrentZoomOffset = 0x228;  // float — the eased/rendered zoom (writing snaps)
		constexpr std::ptrdiff_t kThirdPersonStateIdOffset = 0x50;      // std::uint32_t — must == kThirdPerson(20)
		constexpr std::uint32_t  kThirdPersonStateId = 20;              // RE::CameraState::kThirdPerson

		// Normalized zoom axis [0 .. 2] (NOT meters). Keep above the 0.0 floor (<= floor trips ForceFirstPerson); clamp the authored value into range.
		constexpr float kThirdPersonZoomMin = 0.1f;
		constexpr float kThirdPersonZoomMax = 2.0f;

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
		constexpr float kOrbitStickLookSpeed = 2.2f;    // radians/sec of orbit steer at full right-stick deflection
		constexpr float kOrbitTriggerZoomSpeed = 4.0f;  // meters/sec of radius change at full trigger pull
		constexpr float kOrbitElevLimit = 1.45f;     // ~83°, off the gimbal poles
		constexpr float kOrbitSmoothTime = 0.07f;    // low-pass time constant (s): smaller = snappier, larger = floatier
		constexpr float kOrbitReframeTime = 0.35f;   // slower low-pass used while an auto-reframe glide is in flight
		constexpr float kOrbitFrameFit = 2.5f;       // radius per meter of cast spread — pulls back until the widest cast fits the frame
		constexpr float kOrbitAxisMinSq = 0.04f;     // pair axis shorter than 20 cm = co-located; keep the heading-based azimuth
		constexpr float kPi = 3.14159265f;

		// Smallest absolute angle between two headings (radians, [0, π]).
		float AngularGap(float a_a, float a_b)
		{
			return std::abs(std::remainder(a_a - a_b, 2.0f * kPi));
		}

		bool KeyDown(int a_vk)
		{
			return (GetAsyncKeyState(a_vk) & 0x8000) != 0;
		}

		// --- Gamepad orbit steering, the pad mirror of the mouse scheme: RIGHT stick = mouse drag
		// (orbit angle), LEFT stick = WASD (flies the orbit center), TRIGGERS = wheel (RT zooms in,
		// LT out). Polled like KeyDown; sticks are positions, not deltas, so DriveSceneOrbit
		// integrates them as rate · dt into the same targets the mouse nudges.
		struct PadOrbit
		{
			float lookX{ 0.0f };  // right stick X, deadzoned to [-1, 1], + = right
			float lookY{ 0.0f };  // right stick Y, + = up
			float panF{ 0.0f };   // left stick Y, + = forward
			float panR{ 0.0f };   // left stick X, + = right
			float zoom{ 0.0f };   // RT − LT in [-1, 1], + = zoom in
			bool  steering{ false };  // any axis outside its deadzone this poll
		};

		// Radial deadzone; the live range outside it rescaled so full deflection reads 1.
		void StickAxes(std::int16_t a_x, std::int16_t a_y, float a_deadzone, float& a_outX, float& a_outY)
		{
			const float x = static_cast<float>(a_x);
			const float y = static_cast<float>(a_y);
			const float mag = std::sqrt(x * x + y * y);
			if (mag <= a_deadzone) {
				return;
			}
			const float scale = std::min((mag - a_deadzone) / (32767.0f - a_deadzone), 1.0f) / mag;
			a_outX = x * scale;
			a_outY = y * scale;
		}

		PadOrbit ReadPadOrbit(std::int64_t a_nowMs)
		{
			// XInputGetState on an EMPTY slot stalls in driver enumeration, and DriveSceneOrbit runs
			// several times a frame — after a miss, leave the pad unprobed for a while.
			// driveLock serializes DriveSceneOrbit, so a plain static is safe here.
			static std::int64_t s_retryAtMs = 0;
			PadOrbit out;
			if (a_nowMs < s_retryAtMs) {
				return out;
			}
			XINPUT_STATE state{};
			if (XInputGetState(0, &state) != ERROR_SUCCESS) {
				s_retryAtMs = a_nowMs + 1500;
				return out;
			}
			const auto& pad = state.Gamepad;
			StickAxes(pad.sThumbRX, pad.sThumbRY, static_cast<float>(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE), out.lookX, out.lookY);
			StickAxes(pad.sThumbLX, pad.sThumbLY, static_cast<float>(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE), out.panR, out.panF);
			const auto trigger = [](std::uint8_t a_raw) {
				return a_raw > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ?
				           static_cast<float>(a_raw - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) / (255.0f - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) :
				           0.0f;
			};
			out.zoom = trigger(pad.bRightTrigger) - trigger(pad.bLeftTrigger);
			out.steering = out.lookX != 0.0f || out.lookY != 0.0f || out.panF != 0.0f || out.panR != 0.0f || out.zoom != 0.0f;
			return out;
		}

		// Game thread only. A seated pilot's camera lives in the ship state machine: the on-foot
		// re-entries (ForceFirstPerson / ForceThirdPerson) bail or strand the camera for them, so
		// every path that would force a POV checks this first.
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
				// The full state index too: a pilot's baseline can be a ship exterior state, and
				// restoring that means handing the exact state back, not forcing an on-foot POV.
				baselineStateIndex = 0xFFFFFFFFu;
				for (std::uint32_t i = 0; i < RE::CameraState::kTotal; i++) {
					if (camera->QCameraEquals(static_cast<RE::CameraState>(i))) {
						baselineStateIndex = i;
						break;
					}
				}
				baselineCaptured = true;
			}
		});
	}

	void CameraService::RestoreBaseline()
	{
		SFSE::GetTaskInterface()->AddTask([this]() {
			bool          wantFirst = false;
			std::uint32_t priorState = 0xFFFFFFFFu;
			{
				std::scoped_lock l{ lock };
				// Re-acquired meanwhile, or another imposition still holds the camera, leave it (and keep the baseline for that holder to restore later).
				if (holdCount > 0 || overrideCount > 0) {
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
						// ForceFirstPerson is the ON-FOOT re-entry and bails for a seated pilot —
						// push the engine's own first-person (cockpit) state back directly, the
						// same direct switch the orbit used to leave it.
						camera->SetCameraState(RE::CameraState::kFirstPerson);
						REX::DEBUG("[Camera] restored cockpit first person after scene camera release (pilot)");
					} else {
						camera->ForceFirstPerson();
						REX::DEBUG("[Camera] restored to first person after scene camera release");
					}
				}
				return;
			}
			// Non-first baseline. If it was a state the ENGINE owns (ship exterior, vehicle,
			// furniture, ...), hand that exact state back — forcing the on-foot third person from
			// e.g. a pilot's exterior view strands the camera in the void. Our own imposed states
			// (free-fly / free-walk / vanity) are never a baseline worth returning to, nor is an
			// unknown index: those fall through to the plain third-person restore.
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
				// Explicit because a released override leaves the live camera in an alt state, not third.
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
					REX::DEBUG("[Camera] forced to third person for standalone lock");
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

	void CameraService::KickToThirdPerson()
	{
		SFSE::GetTaskInterface()->AddTask([this]() {
			if (suppressBounce.load(std::memory_order_relaxed)) {
				return;  // a state override owns the camera; don't fight it
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !camera->IsInFirstPerson()) {
				return;  // already out of first person — leave the player's zoom alone
			}
			// A seated pilot's "first person" IS the cockpit view: the on-foot third-person state
			// has no valid framing for a pilot-locked player (the camera strands in the void and
			// nothing restores it — the kick is one-shot by design). Leave the cockpit alone.
			if (PlayerIsPilot()) {
				REX::DEBUG("[Camera] first-person kick skipped — player is piloting a ship");
				return;
			}
			camera->ForceThirdPerson();
			// ForceThirdPerson resets zoom to 0.0 and a zoom at the floor re-trips ForceFirstPerson —
			// seed the pull-back (queues after this task; snap so current never sits at 0).
			SeedThirdPersonZoom(0.0f, /*snapCurrent*/ true);
			REX::DEBUG("[Camera] kicked out of first person (browser opened on the player)");
		});
	}

	void CameraService::SeedThirdPersonZoom(float a_distance, bool a_snapCurrent)
	{
		if (a_distance < 0.0f) {
			return;  // sentinel (< 0): caller asked NOT to seed (e.g. a node-exit teardown pass).
		}
		// Default (a_distance == 0, the common case): open as far OUT as the third-person axis allows.
		// A positive authored "distance" overrides with a specific framing, clamped into range.
		const float seed = (a_distance > 0.0f)
			? std::clamp(a_distance, kThirdPersonZoomMin, kThirdPersonZoomMax)
			: kThirdPersonZoomMax;
		SFSE::GetTaskInterface()->AddTask([this, seed, a_snapCurrent]() {
			if (suppressBounce.load(std::memory_order_relaxed)) {
				return;  // a state override (free-fly / orbit) owns the camera — don't fight it.
			}
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera) {
				return;
			}
			// seed the third-person state object target directly rather than gating on the camera being SETTLED in third person
			// the hold's ForceThirdPerson task ran first (FIFO), and a tween frame must not make us skip the write.
			void* state = camera->cameraStates[RE::CameraState::kThirdPerson];
			if (!state) {
				return;
			}
			// Hard safety before any raw write (a mis-offset write here frees a live state / corrupts the camera): confirm the object is really ThirdPersonState — vtable match AND internal stateId == 20.
			static const REL::Relocation<std::uintptr_t> tpsVtbl{ RE::VTABLE::ThirdPersonState[0] };
			auto* base = reinterpret_cast<std::byte*>(state);
			if (*reinterpret_cast<std::uintptr_t*>(base) != tpsVtbl.address()) {
				return;
			}
			if (*reinterpret_cast<std::uint32_t*>(base + kThirdPersonStateIdOffset) != kThirdPersonStateId) {
				return;
			}
			// Normally write the TARGET only and let the engine glide current -> target. But when the caller asks  to SNAP, also write the CURRENT so it never sits at 0 and re-triggers the 3rd->1st collapse. 
			*reinterpret_cast<float*>(base + kThirdPersonTargetZoomOffset) = seed;
			if (a_snapCurrent) {
				*reinterpret_cast<float*>(base + kThirdPersonCurrentZoomOffset) = seed;
			}
			REX::TRACE("[Camera] third-person start zoom seeded to {} (snapCurrent={})", seed, a_snapCurrent);
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
					// Capture the orbit framing ONCE — a pinned actor's live position wobbles each frame, so
					// reading it per-frame would jitter the camera.
					const float        h = player ? player->data.angle.z : 0.0f;
					const RE::NiPoint3 loc = player ? player->data.location : RE::NiPoint3{};
					// With a cast seed (SetOrbitFrameSubjects): center on the SUBJECTS' midpoint, open the
					// radius until their spread fits the frame, and open SIDE-ON to the cast's long axis so
					// the subjects spread across the frame instead of stacking behind each other (the side
					// nearest the player's heading, so the opening move is the smallest camera arc).
					// Positions are resolved HERE — game thread, after placement moved the cast.
					std::vector<RE::NiPoint3> pts;
					pts.reserve(frameSubjects.size());
					for (const std::uint32_t id : frameSubjects) {
						if (const auto* subject = RE::TESForm::LookupByID<RE::Actor>(id)) {
							pts.push_back(subject->data.location);
						}
					}
					frameSubjects.clear();  // consumed (or dead) — never reused by a later enter
					float azimuth = h;
					float radius = kOrbitDefaultRadius;
					float centerX, centerY, centerZ;
					if (pts.empty()) {
						// No cast seed (direct API use): the player's torso, nudged toward the partner.
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
						// Fit: widest horizontal reach from the center scales the pull-back.
						float spread = 0.0f;
						// Long axis: the most-separated pair (casts are tiny — O(n²) is fine).
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
							const float axisYaw = std::atan2(-axX, axY);  // heading whose forward runs along the pair axis
							const float side1 = axisYaw + 0.5f * kPi;    // the two side-on views
							const float side2 = axisYaw - 0.5f * kPi;
							azimuth = (AngularGap(side1, h) <= AngularGap(side2, h)) ? side1 : side2;
						}
					}
					// GLIDE, don't cut. The framing above goes into the TARGETS; where the RENDERED pose
					// starts depends on what the camera was doing:
					if (orbitDriving.load(std::memory_order_relaxed)) {
						// An orbit is already live (browse orbit, or an earlier scene node) — the user may
						// have steered it, so leave the rendered pose alone and let DriveSceneOrbit low-pass
						// it over at the reframe time constant. lastDriveMs stays live (the driver never
						// stopped; zeroing it would stall the first glide frame).
					} else {
						// Fresh enter (the live camera was third person / vanity): approximate the view the
						// player was just looking through — behind the player at the default ring pose — so
						// the state cut lands near the old view and the framing arrives as a swoop.
						orbitAzimuth = h;
						orbitElevation = kOrbitDefaultElev;
						orbitRadius = kOrbitDefaultRadius;
						orbitCenterX = loc.x;
						orbitCenterY = loc.y;
						orbitCenterZ = loc.z + kOrbitCenterUp;
						lastDriveMs = 0;
					}
					// Azimuth unwrapped to the nearest turn — a dragged browse orbit accumulates whole
					// revolutions, and a raw atan2 target would spin the camera back through them.
					orbitTargetAzimuth = orbitAzimuth + std::remainder(azimuth - orbitAzimuth, 2.0f * kPi);
					orbitTargetElevation = kOrbitDefaultElev;
					orbitTargetRadius = radius;
					orbitCenterTargetX = centerX;
					orbitCenterTargetY = centerY;
					orbitCenterTargetZ = centerZ;
					orbitReframeGlide = true;  // smooth at kOrbitReframeTime until settled (or the user steers)
				}
				orbitDriving.store(true, std::memory_order_relaxed);
				Input::InputService::GetSingleton().SetMouseCapture(true);
				REX::DEBUG("[Camera] scene orbit camera engaged");
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
			REX::DEBUG("[Camera] set to vanity orbit");
		});
	}

	void CameraService::SetOrbitFrameSubjects(std::vector<std::uint32_t> a_actorIds)
	{
		std::scoped_lock l{ driveLock };
		frameSubjects = std::move(a_actorIds);  // consumed by the next scene-orbit enter (see SetLiveCameraState)
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
			SetNativeFreeCam(true);  // enter free cam (state-driven); the engine seeds the pose from the current view
			REX::DEBUG("[Camera] native free camera entered (ToggleFreeCameraMode, state {})", kNativeFreeCamState);
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
				return;  // not on — nothing to do
			}
			SetNativeFreeCam(false);  // clear the engine free-cam status flags 
			// tfc set the player AI-driven (to freeze them while the camera flew) and toggling it off doesn't reliably clear it for a pinned scene participant.
			Player::PlayerControlService::GetSingleton().ClearAIDriven();

			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!suppressBounce.load(std::memory_order_relaxed) && camera) {
				if (handBackToHold) {
					if (!camera->IsInThirdPerson()) {
						camera->ForceThirdPerson();
						REX::DEBUG("[Camera] handed back to third-person hold after native free cam exit");
					}
					SeedThirdPersonZoom(0.0f, /*snapCurrent*/ true);
				} else {
					RestoreBaseline();  // no scene hold active — restore the POV captured before the override
				}
			}

			REX::DEBUG("[Camera] native free camera exited");
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

		// Control scheme (freecam-style orbit): MOUSE / right stick steers the orbit angle, WHEEL /
		// triggers zooms the radius, and WASD / left stick flies the orbit CENTER through the scene
		// (the camera keeps orbiting the moving pivot).
		float mdx = 0.0f, mdy = 0.0f, wheel = 0.0f;
		auto& input = Input::InputService::GetSingleton();
		input.DrainMouseDelta(mdx, mdy);
		input.DrainWheelDelta(wheel);
		const PadOrbit pad = ReadPadOrbit(now);
		// Stick up looks up = camera lowers, matching a mouse drag up (which arrives as -mdy).
		orbitTargetAzimuth -= mdx * kOrbitMouseSens + pad.lookX * kOrbitStickLookSpeed * dt;
		orbitTargetElevation += mdy * kOrbitMouseSens - pad.lookY * kOrbitStickLookSpeed * dt;
		orbitTargetElevation = std::clamp(orbitTargetElevation, -kOrbitElevLimit, kOrbitElevLimit);
		orbitTargetRadius -= wheel * kOrbitWheelStep + pad.zoom * kOrbitTriggerZoomSpeed * dt;  // wheel up / RT (+) = zoom in = smaller radius
		orbitTargetRadius = std::clamp(orbitTargetRadius, kOrbitMinRadius, kOrbitMaxRadius);

		// WASD pans the center in its horizontal plane, relative to the current view: W/S along the camera's
		// horizontal forward (toward/away from where it looks), A/D strafe. The center's height is unchanged.
		const float ch = std::cos(orbitAzimuth), sh = std::sin(orbitAzimuth);
		float panF = pad.panF, panR = pad.panR;
		if (KeyDown('W')) panF += 1.0f;
		if (KeyDown('S')) panF -= 1.0f;
		if (KeyDown('D')) panR += 1.0f;
		if (KeyDown('A')) panR -= 1.0f;
		panF = std::clamp(panF, -1.0f, 1.0f);
		panR = std::clamp(panR, -1.0f, 1.0f);
		if (panF != 0.0f || panR != 0.0f) {
			const float step = kOrbitPanSpeed * dt;
			orbitCenterTargetX += (-sh * panF + ch * panR) * step;  // forward=(-sin,cos), right=(cos,sin)
			orbitCenterTargetY += (ch * panF + sh * panR) * step;
		}

		// Manual steering cancels an in-flight reframe glide: the user's intent wins, and the leftover
		// offset finishes at the snappy input time constant instead of dragging behind their drag.
		if (mdx != 0.0f || mdy != 0.0f || wheel != 0.0f || panF != 0.0f || panR != 0.0f || pad.steering) {
			orbitReframeGlide = false;
		}

		// Low-pass the rendered values toward the targets (frame-rate-independent: alpha = 1 - e^(-dt/tau)).
		// This is what turns raw per-tick input into a smooth glide and absorbs the ~7×/frame tick cadence.
		// While a reframe glide is in flight (scene launch / node retarget) the slower constant carries the
		// camera to the new framing as a visible move; alpha is 0 on the first frame (dt unknown — hold still).
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
			orbitReframeGlide = false;  // settled — input smoothing back to snappy
		}

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
		bool browseHandback = false;
		{
			std::scoped_lock l{ lock };
			if (overrideCount == 0) {
				return;  // not held
			}
			if (--overrideCount != 0) {
				// Still held. If what remains is exactly the browse-orbit hold (scene browser open),
				// a SCENE just released a non-orbit camera (vanity/freefly retargeted the live state
				// away) — hand the camera back to a browse orbit so drag-to-look keeps working instead
				// of leaving the scene's last state running ownerless. A live orbit needs no handback
				// (the count never hit 0, so it just keeps driving), and ReleaseBrowseOrbit clears the
				// flag before calling here, so the browse release itself can't trigger this.
				browseHandback = overrideCount == 1 &&
				                 browseOrbitHeld.load(std::memory_order_relaxed) &&
				                 !orbitDriving.load(std::memory_order_relaxed) &&
				                 !nativeFreeCamActive.load(std::memory_order_relaxed);
			} else {
				released = true;
				suppressBounce.store(false, std::memory_order_relaxed);
			}
		}
		if (browseHandback) {
			SetLiveCameraState(CameraMode::kSceneOrbit);  // no seed staged -> centers on the player
			REX::DEBUG("[Camera] scene camera released while browsing — handed back to the browse orbit");
			return;
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
					REX::DEBUG("[Camera] handed back to third-person hold after state override");
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
			REX::DEBUG("[Camera] player free camera toggled ON (MMB)");
			return;
		}
		playerFreeCamHeld.store(false, std::memory_order_relaxed);
		ReleaseStateOverride();  // hands the camera back to any scene posture / third-person hold / baseline
		REX::DEBUG("[Camera] player free camera toggled OFF (MMB)");
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
			REX::DEBUG("[Camera] player free camera force-exited (scene ended)");
		}
	}

	bool CameraService::EnsureBrowseOrbit(std::vector<std::uint32_t> a_frameSubjects)
	{
		// Aboard a ship that isn't landed, the orbit is unusable: DriveSceneOrbit writes an
		// ABSOLUTE world transform captured once at engage, and in space the cell re-bases /
		// moves under the interior every frame — the camera teleports around the hull and reads
		// as violent spinning. Skip engaging (drag does nothing) and report it, so the browser
		// view can tell the user why; checked before the held flag so a drag after landing
		// mid-browse re-checks and works again. Game thread (OnOrbit).
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			if (auto* ship = player->GetSpaceship()) {
				// NOT IsSpaceshipLanded/Docked: those are placeholder-0 IDs in this commonlib (no
				// address-library entry — first call dies in the ID lookup). IsInSpace is curated.
				if (ship->IsInSpace(true)) {
					REX::DEBUG("[Camera] browse orbit skipped — aboard a ship in space");
					return false;
				}
				REX::DEBUG("[Camera] aboard a landed ship — browse orbit allowed");
			}
		}
		if (browseOrbitHeld.exchange(true, std::memory_order_relaxed)) {
			return true;  // already engaged this browser session
		}
		AcquireStateOverride();
		// If an OSF camera is already moving the mouse-steered orbit (a scene_orbit scene) or the
		// engine free cam (MMB / an authored freefly node), the hold alone is enough — retargeting
		// would re-frame the camera out from under the user mid-scene.
		if (orbitDriving.load(std::memory_order_relaxed) || nativeFreeCamActive.load(std::memory_order_relaxed)) {
			REX::DEBUG("[Camera] browse orbit hold taken (a scene camera is live — not retargeting)");
			return true;
		}
		SetOrbitFrameSubjects(std::move(a_frameSubjects));
		SetLiveCameraState(CameraMode::kSceneOrbit);
		REX::DEBUG("[Camera] browse orbit engaged (scene browser drag)");
		return true;
	}

	void CameraService::ReleaseBrowseOrbit()
	{
		// Clear the flag BEFORE releasing so ReleaseStateOverride's browse handback (scene released,
		// browse remains) can tell this release apart from a scene's and never self-triggers.
		if (browseOrbitHeld.exchange(false, std::memory_order_relaxed)) {
			ReleaseStateOverride();
			REX::DEBUG("[Camera] browse orbit released (scene browser closed)");
		}
	}

	void CameraService::LogCameraTelemetry(const char* a_tag)
	{
		SFSE::GetTaskInterface()->AddTask([a_tag]() {
			auto* camera = RE::PlayerCamera::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!camera || !player) {
				return;
			}
			int stateId = -1;
			for (std::uint32_t i = 0; i < RE::CameraState::kTotal; i++) {
				if (camera->QCameraEquals(static_cast<RE::CameraState>(i))) {
					stateId = static_cast<int>(i);
					break;
				}
			}
			// Third-person internals, behind the same vtable+stateId guard as SeedThirdPersonZoom.
			// -99 = state object missing/mismatched. Orbit yaw @+0x220 per OSF RE camera.state_machine.
			float zoomTarget = -99.0f, zoomCurrent = -99.0f, orbitYaw = -99.0f;
			if (void* state = camera->cameraStates[RE::CameraState::kThirdPerson]) {
				static const REL::Relocation<std::uintptr_t> tpsVtbl{ RE::VTABLE::ThirdPersonState[0] };
				auto* base = reinterpret_cast<std::byte*>(state);
				if (*reinterpret_cast<std::uintptr_t*>(base) == tpsVtbl.address() &&
					*reinterpret_cast<std::uint32_t*>(base + kThirdPersonStateIdOffset) == kThirdPersonStateId) {
					zoomTarget = *reinterpret_cast<float*>(base + kThirdPersonTargetZoomOffset);
					zoomCurrent = *reinterpret_cast<float*>(base + kThirdPersonCurrentZoomOffset);
					orbitYaw = *reinterpret_cast<float*>(base + 0x220);
				}
			}
			REX::DEBUG("[Camera] telemetry[{}]: state={} zoomTgt={:.3f} zoomCur={:.3f} orbitYaw={:.3f} heading={:.3f} pos=({:.2f},{:.2f},{:.2f})",
				a_tag, stateId, zoomTarget, zoomCurrent, orbitYaw,
				player->data.angle.z, player->data.location.x, player->data.location.y, player->data.location.z);
		});
	}

	void CameraService::OnStopAll()
	{
		if (orbitDriving.exchange(false, std::memory_order_relaxed)) {
			Input::InputService::GetSingleton().SetMouseCapture(false);
		}
		{
			std::scoped_lock dl{ driveLock };
			frameSubjects.clear();  // a seed staged for an orbit that never entered must not frame a post-load scene
		}
		if (nativeFreeCamActive.exchange(false, std::memory_order_relaxed)) {
			// Drive the native free cam off so a save/load doesn't strand the player in it.
			SFSE::GetTaskInterface()->AddTask([]() { SetNativeFreeCam(false); });
		}
		playerFreeCamHeld.store(false, std::memory_order_relaxed);
		browseOrbitHeld.store(false, std::memory_order_relaxed);  // counts are dropped wholesale below
		std::scoped_lock l{ lock };
		holdCount = 0;
		overrideCount = 0;
		baselineCaptured = false;
		holdArmed.store(false, std::memory_order_relaxed);
		suppressBounce.store(false, std::memory_order_relaxed);
	}

	void CameraService::OnPostLoad()
	{
		// Runs after OnStopAll zeroed every imposition, so only the LIVE camera state is consulted: if the
		// load left it in a state OSF imposes (scene_orbit=kFreeFly, native freecam=kFreeWalk,
		// vanity_orbit=kAutoVanity), it's a leaked imposition nothing will ever release — recover it.
		// Covers the in-process quickload AND loading a save that was written mid-scene.
		SFSE::GetTaskInterface()->AddTask([]() {
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
			SetNativeFreeCam(false);  // clear the engine free-cam flags too, in case the tfc path leaked
			if (PlayerIsPilot()) {
				camera->SetCameraState(RE::CameraState::kFirstPerson);  // cockpit — Force* strands a seated pilot
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
				REX::TRACE("[Camera] bounced back to third person mid-lock");
			}
		});
	}
}
