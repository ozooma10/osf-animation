#include "Util/NativeMainThreadQueue.h"
#include "Input/InputService.h"
#include "Input/SceneKeymap.h"
#include "API/OSFEngineAPI.h"

#include "SFSE/InputMap.h"
#include "REL/THook.h"

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

		std::atomic_bool          g_active{ false };
		std::optional<REL::THookVFT<PerformInputProcessing_t>> g_inputHook;

		std::mutex                              g_grantLock;    // guards g_grant + g_verbHandler
		Grant                                   g_grant;
		std::function<void(Verb, const Grant&)> g_verbHandler;

		// The engine won't route input to a self-driven FreeFly state while the player is a pinned participant.
		std::atomic_bool   g_captureMouse{ false };
		std::atomic<float> g_mouseDx{ 0.0f };
		std::atomic<float> g_mouseDy{ 0.0f };
		std::atomic<float> g_mouseWheel{ 0.0f };  // notches (+ = up / zoom in)
		// Consumer-selected routing; the engine has no presentation or pointer-gesture policy.
		std::atomic<std::uint32_t> g_inputRouting{0};
		std::atomic_bool   g_nativeFreeCamGamepad{ false };
		std::atomic<float> g_padLeftX{ 0.0f };
		std::atomic<float> g_padLeftY{ 0.0f };
		std::atomic<float> g_padRightX{ 0.0f };
		std::atomic<float> g_padRightY{ 0.0f };
		std::atomic<float> g_padLeftTrigger{ 0.0f };
		std::atomic<float> g_padRightTrigger{ 0.0f };
		std::atomic_bool   g_padLeftShoulder{ false };
		std::atomic_bool   g_padRightShoulder{ false };
		// MouseMoveEvent: int32 x,y deltas follow the IDEvent base (0x38).
		constexpr std::ptrdiff_t kMouseMoveDeltaOffset = 0x38;
		// ThumbstickEvent (not modeled by CommonLibSF): idCode 0x0B/0x0C = left/right; float x/y at +0x38/+0x3C.
		constexpr std::ptrdiff_t kThumbstickIdOffset = 0x30;
		constexpr std::ptrdiff_t kThumbstickXOffset = 0x38;
		constexpr std::ptrdiff_t kThumbstickYOffset = 0x3C;
		constexpr std::int32_t   kLeftThumbstickId = 0x0B;
		constexpr std::int32_t   kRightThumbstickId = 0x0C;
		constexpr std::int32_t   kLeftTriggerId = 0x09;
		constexpr std::int32_t   kRightTriggerId = 0x0A;
		constexpr std::int32_t   kLeftShoulderId = 0x0100;
		constexpr std::int32_t   kRightShoulderId = 0x0200;
		// Wheel = kMouse ButtonEvents: 0x800 up / 0x900 down, value = signed WHEEL_DELTA (±120), then a 0-value release.
		constexpr std::int32_t kMouseWheelUpId = 0x800;
		constexpr std::int32_t kMouseWheelDownId = 0x900;
		constexpr float        kWheelNotch = 120.0f;
		// Mouse button idCode = button index (0 = left, 1 = right, 2 = middle, 3/4 = X1/X2).
		constexpr std::int32_t kMouseMiddleId = 2;

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

		// RE::UI::VTABLE (AddressLib 475439) is not in base-declaration order.
		constexpr std::size_t kReceiverVtblIndex = 10;

		// idCode and OSF Settings keys both use native Windows VK codes.
		Verb VerbForKeyboard(std::int32_t a_idCode)
		{
			return g_sceneKeymap.Lookup(static_cast<std::uint32_t>(a_idCode));
		}

		Verb VerbForGamepad(std::int32_t a_idCode)
		{
			const auto key = SFSE::InputMap::GamepadMaskToKeycode(static_cast<std::uint32_t>(a_idCode));
			return key == SFSE::InputMap::kGamepadButtonOffset_RIGHT_THUMB ? Verb::kFreecam : Verb::kNone;
		}

		// Press edge only. Hot path stays light: copy the grant and post; scene work runs on the game thread.
		void MaybeDispatch(const RE::ButtonEvent* a_event)
		{
			if (a_event->value == 0.0f || a_event->heldDownSecs != 0.0f) {
				return;
			}
			Verb verb = Verb::kNone;
			if (a_event->deviceType == RE::InputEvent::DeviceType::kKeyboard) {
				verb = VerbForKeyboard(a_event->idCode);
			} else if (a_event->deviceType == RE::InputEvent::DeviceType::kMouse &&
					   a_event->idCode == kMouseMiddleId) {
				verb = Verb::kFreecam;
			} else if (a_event->deviceType == RE::InputEvent::DeviceType::kGamepad) {
				verb = VerbForGamepad(a_event->idCode);  // R3 mirrors MMB
			}
			if (verb == Verb::kNone) {
				return;
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
				return;
			}
			if (verb == Verb::kEnd && grant.locked) {
				return;  // AAF isRootLocked analogue
			}
			Util::NativeMainThreadQueue::Post([handler, verb, grant]() { handler(verb, grant); });
		}

		bool MenuOwnsInput()
		{
			if (auto* main = RE::Main::GetSingleton(); main && main->isGameMenuPaused) {
				return true;
			}
			if (auto* ui = RE::UI::GetSingleton()) {
				// Interned once; leaked so process teardown never releases into a destroyed string pool.
				static const auto* const consoleMenu = new RE::BSFixedString(RE::Console::MENU_NAME);
				return ui->IsMenuOpen(*consoleMenu);
			}
			return false;
		}

		void Thunk(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_queueHead)
		{
			const bool active = g_active.load(std::memory_order_relaxed);
			const bool capture = g_captureMouse.load(std::memory_order_relaxed);
			const auto routing = g_inputRouting.load(std::memory_order_relaxed);
			const bool nativeFreeCamGamepad = g_nativeFreeCamGamepad.load(std::memory_order_relaxed);
			// Idle frames skip the menu query; every consumer below is gated on one of these.
			const bool engaged = active || capture || routing;
			const bool menuOwns = engaged && MenuOwnsInput();
			if (engaged && !menuOwns) {
				for (const auto* event = a_queueHead; event; event = event->next) {
					const auto et = event->eventType;
					if (capture && et == RE::InputEvent::EventType::kDeviceConnect) {
						ResetGamepadState();
					}
					if (capture && et == RE::InputEvent::EventType::kThumbstick &&
						event->deviceType == RE::InputEvent::DeviceType::kGamepad) {
						CaptureThumbstick(event);
					}
					// Requested thumbstick consumption must precede the receiver, where movement dispatch occurs.
					if ((routing & API::ConsumeThumbsticks) &&
						!(nativeFreeCamGamepad && (routing & API::AllowNativeFreeCamera)) && et == RE::InputEvent::EventType::kThumbstick &&
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
					const bool steer = !(routing & API::SuppressRawMouse);
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
						if (steer && (btn->idCode == kMouseWheelUpId || btn->idCode == kMouseWheelDownId)) {
							g_mouseWheel.fetch_add(btn->value / kWheelNotch, std::memory_order_relaxed);
						}
					}
				}
			}
			(*g_inputHook)(a_this, a_queueHead);
			// Consumers may also request that downstream receivers do not reuse these events.
			if ((routing & API::ConsumeGamepadAfterDispatch) && !menuOwns) {
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

		// The live receiver vptr must equal AddressLib's vtable before anything writes to the UI object.
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
		if (g_inputHook && g_inputHook->GetEnabled()) {
			return true;
		}
		if (!VerifyUiLayout()) {
			REX::ERROR("[Input] refusing to install (UI layout guard failed) — native input channel unavailable");
			return false;
		}

		// Slot 1 = PerformInputProcessing (0 = destructor).
		g_inputHook.emplace("[Input] UI::PerformInputProcessing", RE::UI::VTABLE[kReceiverVtblIndex], 1, &Thunk);
		if (!g_inputHook->Enable()) {
			return false;
		}
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
		SetInputRouting(0);
		SetMouseCapture(false);
	}

	void InputService::SetMouseCapture(bool a_on)
	{
		g_captureMouse.store(a_on, std::memory_order_relaxed);
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
			return;  // inactive input must not bank deltas
		}
		g_mouseDx.fetch_add(a_dx, std::memory_order_relaxed);
		g_mouseDy.fetch_add(a_dy, std::memory_order_relaxed);
		g_mouseWheel.fetch_add(a_wheel, std::memory_order_relaxed);
	}

	void InputService::SetInputRouting(std::uint32_t flags)
    {
        constexpr auto supported = API::SuppressRawMouse | API::ConsumeThumbsticks |
            API::ConsumeGamepadAfterDispatch | API::AllowNativeFreeCamera;
        g_inputRouting.store(flags & supported, std::memory_order_relaxed);
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
