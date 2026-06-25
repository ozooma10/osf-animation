#include "Input/InputService.h"

#include "RE/B/BSInputEventReceiver.h"
#include "RE/B/BSInputEventUser.h"
#include "RE/U/UI.h"

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

		// Index of the BSInputEventReceiver vtable in RE::UI::VTABLE (AddressLib id 475439). The IDs_VTABLE array is NOT in base-declaration order;
		constexpr std::size_t kReceiverVtblIndex = 10;

		// Default keyboard keymap. For device kKeyboard the idCode is a Windows VK code. 
		// @TODO: a settings.json keymap override
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

		// On a keyboard PRESS edge, map -> verb, capability-gate against the active grant, then post the verb to the game thread via the registered handler. 
		// Keeps the input hot path lock-free: the thunk only copies the grant + posts a task; the heavy scene work runs on the game thread.
		void MaybeDispatch(const RE::ButtonEvent* a_event)
		{
			if (a_event->deviceType != RE::InputEvent::DeviceType::kKeyboard) {
				return;  // v1 keyboard-only; gamepad idCodes pending capture
			}
			if (a_event->value == 0.0f || a_event->heldDownSecs != 0.0f) {
				return;  // press edge only (no release / held-repeat verbs in v1)
			}
			const Verb verb = VerbForKeyboard(a_event->idCode);

			REX::INFO("InputService: input event device {} idCode {:#x} value {:.2f} -> verb {}",
				static_cast<std::uint32_t>(a_event->deviceType), a_event->idCode, a_event->value, VerbName(verb));

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
				return;  // scene didn't grant this capability
			}
			if (verb == Verb::kEnd && grant.locked) {
				return;  // scene forbids player-end (AAF isRootLocked analogue)
			}
			SFSE::GetTaskInterface()->AddTask([handler, verb, grant]() { handler(verb, grant); });
		}

		void Thunk(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_queueHead)
		{
			if (g_active.load(std::memory_order_relaxed)) {
				for (const auto* event = a_queueHead; event; event = event->next) {
					if (event->eventType == RE::InputEvent::EventType::kButton) {
						MaybeDispatch(static_cast<const RE::ButtonEvent*>(event));
					}
				}
			}
			// ALWAYS forward the unmodified queue. This hook reads input; it never consumes / injects.
			g_original(a_this, a_queueHead);
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
			REX::ERROR("InputService: RE::UI singleton is null; layout unverifiable");
			return false;
		}

		// Cross-check the compiled UI base offsets against the running binary before anything writes to the UI object: 
		// the vptr of the live BSInputEventReceiver subobject must be exactly the vtable AddressLib reports for it.
		auto*      receiver = static_cast<RE::BSInputEventReceiver*>(ui);
		const auto liveVptr = *reinterpret_cast<const std::uintptr_t*>(receiver);
		const REL::Relocation<std::uintptr_t> vtbl{ RE::UI::VTABLE[kReceiverVtblIndex] };
		if (liveVptr != vtbl.address()) {
			REX::ERROR("InputService: UI layout guard FAILED — live BSInputEventReceiver vptr {:#x} != "
					   "AddressLib UI::VTABLE[{}] {:#x} (CommonLibSF layout or address library stale for "
					   "this game version); dumping all entries:",
				liveVptr, kReceiverVtblIndex, vtbl.address());
			for (std::size_t i = 0; i < RE::UI::VTABLE.size(); ++i) {
				const REL::Relocation<std::uintptr_t> entry{ RE::UI::VTABLE[i] };
				REX::ERROR("InputService:   UI::VTABLE[{:2}] (ID {}) = {:#x}{}",
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
			REX::ERROR("InputService: refusing to install (UI layout guard failed) — native input channel unavailable");
			return false;
		}

		// Slot 1 is PerformInputProcessing (0 is the virtual destructor); 
		REL::Relocation<std::uintptr_t> vtbl{ RE::UI::VTABLE[kReceiverVtblIndex] };
		const auto                      original = vtbl.write_vfunc(1, &Thunk);
		g_original = reinterpret_cast<PerformInputProcessing_t*>(original);
		g_installed = true;
		REX::INFO("InputService: installed vfunc hook on UI::PerformInputProcessing");
		return true;
	}

	bool InputService::Installed() const
	{
		return g_installed;
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
		REX::INFO("InputService: director channel engaged for scene {:#010x} (capabilities {:#x}, locked {})", a_grant.handle, a_grant.capabilities, a_grant.locked);
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
			REX::INFO("InputService: director channel released for scene {:#010x}", a_handle);
		}
	}

	void InputService::OnStopAll()
	{
		{
			std::scoped_lock l{ g_grantLock };
			g_grant = Grant{};
		}
		g_active.store(false, std::memory_order_relaxed);
	}
}
