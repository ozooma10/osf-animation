#include "Input/InputService.h"

#include "SFSE/InputMap.h"

#include "RE/B/BSInputEventReceiver.h"
#include "RE/B/BSInputEventUser.h"
#include "RE/C/Console.h"
#include "RE/M/Main.h"
#include "RE/U/UI.h"

#include <algorithm>

namespace OSF::Input
{
	namespace
	{
		// `this` for PerformInputProcessing is the BSInputEventReceiver subobject (UI + 0x10), not the UI base.
		using PerformInputProcessing_t = void(RE::BSInputEventReceiver*, const RE::InputEvent*);

		std::atomic_bool          g_active{ false };     // a grant is live -> verb dispatch armed
		bool                      g_installed{ false };
		PerformInputProcessing_t* g_original{ nullptr };

		std::mutex                              g_grantLock;    // guards g_grant + g_verbHandler
		Grant                                   g_grant;
		std::function<void(Verb, const Grant&)> g_verbHandler;

		// Mouse-look delta passthrough for the self-driven scene-orbit camera. While capture is on, the
		// hook accumulates MouseMoveEvent deltas for CameraService to drain (the engine won't route input
		// to a self-driven free-fly state while the player is a pinned scene participant).
		std::atomic_bool   g_captureMouse{ false };
		std::atomic<float> g_mouseDx{ 0.0f };
		std::atomic<float> g_mouseDy{ 0.0f };
		std::atomic<float> g_mouseWheel{ 0.0f };  // net wheel ticks since the last drain (+ = up / zoom in)
		// UI-cursor mode (scene browser visible): look/wheel deltas only accumulate while the LEFT
		// button is held (click-drag orbits; free movement steers the cursor, free wheel scrolls the UI).
		std::atomic_bool   g_uiCursorVisible{ false };
		std::atomic_bool   g_orbitDragHeld{ false };  // LMB is down (tracked only while capture is on)
		std::atomic_bool   g_nativeFreeCamGamepad{ false };  // let engine TFC consume pad axes while the browser is visible
		std::atomic<float> g_padLeftX{ 0.0f };
		std::atomic<float> g_padLeftY{ 0.0f };
		std::atomic<float> g_padRightX{ 0.0f };
		std::atomic<float> g_padRightY{ 0.0f };
		std::atomic<float> g_padLeftTrigger{ 0.0f };
		std::atomic<float> g_padRightTrigger{ 0.0f };
		std::atomic_bool   g_padLeftShoulder{ false };
		std::atomic_bool   g_padRightShoulder{ false };
		// MouseMoveEvent is an IDEvent (0x38) subclass; the two raw axis deltas (int32 x,y) follow it.
		constexpr std::ptrdiff_t kMouseMoveDeltaOffset = 0x38;
		// ThumbstickEvent is not yet modeled by CommonLibSF. Runtime-proven layout: IDEvent idCode
		// 0x0B/0x0C identifies left/right; normalized float x/y follow IDEvent at +0x38/+0x3C.
		constexpr std::ptrdiff_t kThumbstickIdOffset = 0x30;
		constexpr std::ptrdiff_t kThumbstickXOffset = 0x38;
		constexpr std::ptrdiff_t kThumbstickYOffset = 0x3C;
		constexpr std::int32_t   kLeftThumbstickId = 0x0B;
		constexpr std::int32_t   kRightThumbstickId = 0x0C;
		constexpr std::int32_t   kLeftTriggerId = 0x09;
		constexpr std::int32_t   kRightTriggerId = 0x0A;
		constexpr std::int32_t   kLeftShoulderId = 0x0100;
		constexpr std::int32_t   kRightShoulderId = 0x0200;
		// The mouse wheel arrives as ButtonEvents on the kMouse device (confirmed in-game): idCode 0x800 =
		// wheel up, 0x900 = wheel down, and value is the signed Windows WHEEL_DELTA (+120 up, -120 down) with
		// a 0-value release event after each. We normalize by 120 so the accumulator counts notches.
		constexpr std::int32_t kMouseWheelUpId = 0x800;
		constexpr std::int32_t kMouseWheelDownId = 0x900;
		constexpr float        kWheelNotch = 120.0f;
		// Mouse buttons arrive as ButtonEvents on the kMouse device with idCode = button index
		// (0 = left, 1 = right, 2 = middle, 3/4 = X1/X2), distinct from the 0x800/0x900 wheel codes.
		constexpr std::int32_t kMouseLeftId = 0;    // LMB -> drag-to-orbit while a UI cursor is up
		constexpr std::int32_t kMouseMiddleId = 2;  // MMB -> toggle free camera

		void ResetGamepadState()
		{
			g_padLeftX.store(0.0f, std::memory_order_relaxed);
			g_padLeftY.store(0.0f, std::memory_order_relaxed);
			g_padRightX.store(0.0f, std::memory_order_relaxed);
			g_padRightY.store(0.0f, std::memory_order_relaxed);
			g_padLeftTrigger.store(0.0f, std::memory_order_relaxed);
			g_padRightTrigger.store(0.0f, std::memory_order_relaxed);
			g_padLeftShoulder.store(false, std::memory_order_relaxed);
			g_padRightShoulder.store(false, std::memory_order_relaxed);
		}

		void CaptureThumbstick(const RE::InputEvent* a_event)
		{
			const auto* bytes = reinterpret_cast<const std::byte*>(a_event);
			const auto id = *reinterpret_cast<const std::int32_t*>(bytes + kThumbstickIdOffset);
			const float x = *reinterpret_cast<const float*>(bytes + kThumbstickXOffset);
			const float y = *reinterpret_cast<const float*>(bytes + kThumbstickYOffset);
			if (id == kLeftThumbstickId) {
				g_padLeftX.store(x, std::memory_order_relaxed);
				g_padLeftY.store(y, std::memory_order_relaxed);
			} else if (id == kRightThumbstickId) {
				g_padRightX.store(x, std::memory_order_relaxed);
				g_padRightY.store(y, std::memory_order_relaxed);
			}
		}

		void CaptureGamepadButton(const RE::ButtonEvent* a_event)
		{
			const float value = std::clamp(a_event->value, 0.0f, 1.0f);
			switch (a_event->idCode) {
			case kLeftTriggerId:
				g_padLeftTrigger.store(value, std::memory_order_relaxed);
				break;
			case kRightTriggerId:
				g_padRightTrigger.store(value, std::memory_order_relaxed);
				break;
			case kLeftShoulderId:
				g_padLeftShoulder.store(value != 0.0f, std::memory_order_relaxed);
				break;
			case kRightShoulderId:
				g_padRightShoulder.store(value != 0.0f, std::memory_order_relaxed);
				break;
			default:
				break;
			}
		}

		// Index of the BSInputEventReceiver vtable in RE::UI::VTABLE (AddressLib id 475439). The IDs_VTABLE array is NOT in base-declaration order;
		constexpr std::size_t kReceiverVtblIndex = 10;

		// Default keyboard keymap. For device kKeyboard the idCode is a Windows VK code.
		// @TODO: a keymap override (would live in the OSF UI settings schema, src/API/UISettings.cpp)
		Verb VerbForKeyboard(std::int32_t a_idCode)
		{
			switch (a_idCode) {
			case 0x20: return Verb::kAdvance;     // Space
			case 0xBB:                            // OEM '=' / '+'
			case 0x6B: return Verb::kSpeedUp;     // Numpad +
			case 0xBD:                            // OEM '-'
			case 0x6D: return Verb::kSpeedDown;   // Numpad -
			case 0x30: return Verb::kSpeedReset;  // '0'
			case 0x50: return Verb::kPause;       // 'P'
			case 0x23: return Verb::kEnd;         // End
			default: return Verb::kNone;
			}
		}

		Verb VerbForGamepad(std::int32_t a_idCode)
		{
			const auto key = SFSE::InputMap::GamepadMaskToKeycode(static_cast<std::uint32_t>(a_idCode));
			return key == SFSE::InputMap::kGamepadButtonOffset_RIGHT_THUMB ? Verb::kFreecam : Verb::kNone;
		}

		// On a keyboard PRESS edge, map -> verb, capability-gate against the active grant, then post the verb to the game thread via the registered handler. 
		// Keeps the input hot path lock-free: the thunk only copies the grant + posts a task; the heavy scene work runs on the game thread.
		void MaybeDispatch(const RE::ButtonEvent* a_event)
		{
			if (a_event->value == 0.0f || a_event->heldDownSecs != 0.0f) {
				return;  // press edge only (no release / held-repeat verbs in v1)
			}
			Verb verb = Verb::kNone;
			if (a_event->deviceType == RE::InputEvent::DeviceType::kKeyboard) {
				verb = VerbForKeyboard(a_event->idCode);
			} else if (a_event->deviceType == RE::InputEvent::DeviceType::kMouse &&
					   a_event->idCode == kMouseMiddleId) {
				verb = Verb::kFreecam;  // MMB toggles native free camera while a scene grants it
			} else if (a_event->deviceType == RE::InputEvent::DeviceType::kGamepad) {
				verb = VerbForGamepad(a_event->idCode);  // R3 mirrors MMB
			}
			if (verb == Verb::kNone) {
				return;  // unmapped key/button
			}

			Grant                                   grant;
			std::function<void(Verb, const Grant&)> handler;
			{
				std::scoped_lock l{ g_grantLock };
				if (g_grant.handle == 0 || !g_verbHandler) {
					return;
				}
				grant = g_grant;
				handler = g_verbHandler;
			}
			if ((grant.capabilities & RequiredCapability(verb)) == 0) {
				return;  // scene didn't grant this capability
			}
			if (verb == Verb::kEnd && grant.locked) {
				return;  // scene forbids player-end (AAF isRootLocked analogue)
			}
			SFSE::GetTaskInterface()->AddTask([handler, verb, grant]() { handler(verb, grant); });
		}

		// if menu/console owns input
		bool MenuOwnsInput()
		{
			if (auto* main = RE::Main::GetSingleton(); main && main->isGameMenuPaused) {
				return true;
			}
			if (auto* ui = RE::UI::GetSingleton()) {
				return ui->IsMenuOpen(RE::Console::MENU_NAME);
			}
			return false;
		}

		void Thunk(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_queueHead)
		{
			const bool active = g_active.load(std::memory_order_relaxed);
			const bool capture = g_captureMouse.load(std::memory_order_relaxed);
			const bool uiVisible = g_uiCursorVisible.load(std::memory_order_relaxed);
			const bool nativeFreeCamGamepad = g_nativeFreeCamGamepad.load(std::memory_order_relaxed);
			const bool menuOwns = MenuOwnsInput();
			// don't route any input while a menu/console owns it. The engine still receives the queue below.
			if ((active || capture || uiVisible) && !menuOwns) {
				for (const auto* event = a_queueHead; event; event = event->next) {
					const auto et = event->eventType;
					if (capture && et == RE::InputEvent::EventType::kDeviceConnect) {
						ResetGamepadState();
					}
					if (capture && et == RE::InputEvent::EventType::kThumbstick &&
						event->deviceType == RE::InputEvent::DeviceType::kGamepad) {
						CaptureThumbstick(event);
					}
					// Browser on screen: kill THUMBSTICK events BEFORE the forward — the player's
					// movement dispatch runs inside g_original (post-forward flagging demonstrably
					// did not stop the walking). The orbit snapshot above has already retained them.
					// Gamepad BUTTONS deliberately pass through unflagged: OSF UI's FocusMenu
					// (also inside g_original) must see them to apply its engine-side capture gate;
					// OSF UI's independent gamepad session maps D-pad/A/B for the browser,
					// and gameplay button verbs are gated by OSF UI's ControlLayer flags (proven
					// for buttons — only stick movement ignores them). Buttons are swept after
					// the forward below as belt-and-suspenders.
					if (uiVisible && !nativeFreeCamGamepad && et == RE::InputEvent::EventType::kThumbstick &&
						event->deviceType == RE::InputEvent::DeviceType::kGamepad) {
						const_cast<RE::InputEvent*>(event)->status = RE::InputEvent::Status::kStop;
						continue;
					}
					if (active && et == RE::InputEvent::EventType::kButton) {
						MaybeDispatch(static_cast<const RE::ButtonEvent*>(event));
					}
					if (capture && et == RE::InputEvent::EventType::kButton &&
						event->deviceType == RE::InputEvent::DeviceType::kGamepad) {
						CaptureGamepadButton(static_cast<const RE::ButtonEvent*>(event));
					}
					if (!capture) {
						continue;
					}
					// UI-cursor mode: with the browser on screen, only a held LMB routes the mouse to the
					// orbit (click-drag). Otherwise the mouse belongs to the UI cursor / list scroll.
					const bool steer = !g_uiCursorVisible.load(std::memory_order_relaxed) ||
					                   g_orbitDragHeld.load(std::memory_order_relaxed);
					if (et == RE::InputEvent::EventType::kMouseMove) {
						if (steer) {
							const auto* d = reinterpret_cast<const std::int32_t*>(
								reinterpret_cast<const std::byte*>(event) + kMouseMoveDeltaOffset);
							g_mouseDx.fetch_add(static_cast<float>(d[0]), std::memory_order_relaxed);
							g_mouseDy.fetch_add(static_cast<float>(d[1]), std::memory_order_relaxed);
						}
					} else if (et == RE::InputEvent::EventType::kButton) {
						const auto* btn = static_cast<const RE::ButtonEvent*>(event);
						if (btn->deviceType != RE::InputEvent::DeviceType::kMouse) {
							continue;
						}
						// LMB edge tracking (press = value != 0; the release event carries value 0).
						if (btn->idCode == kMouseLeftId) {
							g_orbitDragHeld.store(btn->value != 0.0f, std::memory_order_relaxed);
						}
						// Mouse wheel: value is already signed by direction (+up / -down), so accumulate it
						// for either wheel idCode and normalize to notches. (The 0-value release adds nothing.)
						if (steer && (btn->idCode == kMouseWheelUpId || btn->idCode == kMouseWheelDownId)) {
							g_mouseWheel.fetch_add(btn->value / kWheelNotch, std::memory_order_relaxed);
						}
					}
				}
			}
			// Forward the queue: thumbsticks ride along flagged kStop (consumed above), while gamepad
			// buttons pass through live so the menu walk inside — OSF UI's FocusMenu — can apply its
			// capture gate. Browser navigation comes from OSF UI's independent gamepad session.
			g_original(a_this, a_queueHead);
			// Post-forward sweep: flag the remaining gamepad events (buttons) too, in case anything
			// downstream of the UI receiver still looks at this queue. Gameplay button verbs are
			// primarily gated by OSF UI's ControlLayer flags; this is defense in depth.
			if (uiVisible && !menuOwns) {
				for (const auto* event = a_queueHead; event; event = event->next) {
					if (event->deviceType == RE::InputEvent::DeviceType::kGamepad) {
						const_cast<RE::InputEvent*>(event)->status = RE::InputEvent::Status::kStop;
					}
				}
			}
		}
	}

	InputService& InputService::GetSingleton()
	{
		static InputService instance;
		return instance;
	}

	bool InputService::VerifyUiLayout()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("[Input] RE::UI singleton is null; layout unverifiable");
			return false;
		}

		// Cross-check the compiled UI base offsets against the running binary before anything writes to the UI object: 
		// the vptr of the live BSInputEventReceiver subobject must be exactly the vtable AddressLib reports for it.
		auto*      receiver = static_cast<RE::BSInputEventReceiver*>(ui);
		const auto liveVptr = *reinterpret_cast<const std::uintptr_t*>(receiver);
		const REL::Relocation<std::uintptr_t> vtbl{ RE::UI::VTABLE[kReceiverVtblIndex] };
		if (liveVptr != vtbl.address()) {
			REX::ERROR("[Input] UI layout guard FAILED — live BSInputEventReceiver vptr {:#x} != "
					   "AddressLib UI::VTABLE[{}] {:#x} (CommonLibSF layout or address library stale for "
					   "this game version); dumping all entries:",
				liveVptr, kReceiverVtblIndex, vtbl.address());
			for (std::size_t i = 0; i < RE::UI::VTABLE.size(); ++i) {
				const REL::Relocation<std::uintptr_t> entry{ RE::UI::VTABLE[i] };
				REX::ERROR("[Input]   UI::VTABLE[{:2}] (ID {}) = {:#x}{}",
					i, RE::UI::VTABLE[i].id(), entry.address(),
					entry.address() == liveVptr ? "  <-- matches live vptr" : "");
			}
			return false;
		}
		return true;
	}

	bool InputService::Install()
	{
		if (g_installed) {
			return true;
		}
		if (!VerifyUiLayout()) {
			REX::ERROR("[Input] refusing to install (UI layout guard failed) — native input channel unavailable");
			return false;
		}

		// Slot 1 is PerformInputProcessing (0 is the virtual destructor); 
		REL::Relocation<std::uintptr_t> vtbl{ RE::UI::VTABLE[kReceiverVtblIndex] };
		const auto                      original = vtbl.write_vfunc(1, &Thunk);
		g_original = reinterpret_cast<PerformInputProcessing_t*>(original);
		g_installed = true;
		REX::DEBUG("[Input] installed vfunc hook on UI::PerformInputProcessing");
		return true;
	}

	void InputService::SetVerbHandler(std::function<void(Verb, const Grant&)> a_handler)
	{
		std::scoped_lock l{ g_grantLock };
		g_verbHandler = std::move(a_handler);
	}

	void InputService::Engage(const Grant& a_grant)
	{
		{
			std::scoped_lock l{ g_grantLock };
			g_grant = a_grant;
		}
		g_active.store(true, std::memory_order_relaxed);
		REX::DEBUG("[Input] director channel engaged for scene {:#010x} (capabilities {:#x}, locked {})", a_grant.handle, a_grant.capabilities, a_grant.locked);
	}

	void InputService::Release(std::int32_t a_handle)
	{
		bool cleared = false;
		{
			std::scoped_lock l{ g_grantLock };
			if (g_grant.handle == a_handle) {
				g_grant = Grant{};
				cleared = true;
			}
		}
		if (cleared) {
			g_active.store(false, std::memory_order_relaxed);
			REX::DEBUG("[Input] director channel released for scene {:#010x}", a_handle);
		}
	}

	void InputService::OnStopAll()
	{
		{
			std::scoped_lock l{ g_grantLock };
			g_grant = Grant{};
		}
		g_active.store(false, std::memory_order_relaxed);
		g_nativeFreeCamGamepad.store(false, std::memory_order_relaxed);
		SetMouseCapture(false);
	}

	void InputService::SetMouseCapture(bool a_on)
	{
		g_captureMouse.store(a_on, std::memory_order_relaxed);
		g_orbitDragHeld.store(false, std::memory_order_relaxed);  // never carry a held drag across engage/release
		if (!a_on) {
			g_mouseDx.store(0.0f, std::memory_order_relaxed);
			g_mouseDy.store(0.0f, std::memory_order_relaxed);
			g_mouseWheel.store(0.0f, std::memory_order_relaxed);
			ResetGamepadState();
		}
	}

	void InputService::InjectOrbitDelta(float a_dx, float a_dy, float a_wheel)
	{
		if (!g_captureMouse.load(std::memory_order_relaxed)) {
			return;  // no scene-orbit camera driving — a stray UI drag must not bank deltas
		}
		// View CSS px -> raw-mouse-unit axis: the view is 1280 logical px wide and a full-width
		// drag should read as roughly a half orbit (π / (1280 · kOrbitMouseSens 0.004) ≈ 0.6).
		constexpr float kUiDragScale = 0.6f;
		g_mouseDx.fetch_add(a_dx * kUiDragScale, std::memory_order_relaxed);
		g_mouseDy.fetch_add(a_dy * kUiDragScale, std::memory_order_relaxed);
		g_mouseWheel.fetch_add(a_wheel, std::memory_order_relaxed);  // already in notches
	}

	void InputService::SetUiCursorVisible(bool a_on)
	{
		g_uiCursorVisible.store(a_on, std::memory_order_relaxed);
		if (a_on) {
			// The cursor just appeared: any in-flight drag state is stale (the click that opened the
			// browser must not start steering the camera).
			g_orbitDragHeld.store(false, std::memory_order_relaxed);
		}
	}

	void InputService::SetNativeFreeCamGamepad(bool a_on)
	{
		g_nativeFreeCamGamepad.store(a_on, std::memory_order_relaxed);
	}

	void InputService::DrainMouseDelta(float& a_dx, float& a_dy)
	{
		a_dx = g_mouseDx.exchange(0.0f, std::memory_order_relaxed);
		a_dy = g_mouseDy.exchange(0.0f, std::memory_order_relaxed);
	}

	void InputService::DrainWheelDelta(float& a_wheel)
	{
		a_wheel = g_mouseWheel.exchange(0.0f, std::memory_order_relaxed);
	}

	GamepadState InputService::ReadGamepadState() const
	{
		return {
			.leftX = g_padLeftX.load(std::memory_order_relaxed),
			.leftY = g_padLeftY.load(std::memory_order_relaxed),
			.rightX = g_padRightX.load(std::memory_order_relaxed),
			.rightY = g_padRightY.load(std::memory_order_relaxed),
			.leftTrigger = g_padLeftTrigger.load(std::memory_order_relaxed),
			.rightTrigger = g_padRightTrigger.load(std::memory_order_relaxed),
			.leftShoulder = g_padLeftShoulder.load(std::memory_order_relaxed),
			.rightShoulder = g_padRightShoulder.load(std::memory_order_relaxed),
		};
	}
}
