#include "UI/HudMessage.h"

#include <string>

namespace OSF::UI::HudMessage
{
	namespace
	{
		std::atomic_bool g_debugEnabled{ false };
	}

	namespace
	{
		// Build and fire the ShowHUDMessageEvent. a_isWarning routes the HUD to its warning
		// styling (used for errors). canThrottle lets the HUD coalesce a burst.
		void Fire(std::string_view a_text, bool a_isWarning)
		{
			auto* source = RE::ShowHUDMessageEvent::GetEventSource();
			if (!source) {
				REX::DEBUG("[UI] no ShowHUDMessageEvent source yet — dropped '{}'", a_text);
				return;
			}
			// {text, sound = none, canThrottle = true, isWarning} — matches the Papyrus
			// Debug.Notification native (1.16.244).
			const std::string       text{ a_text };
			RE::ShowHUDMessageEvent  ev{};
			ev.text = RE::BSFixedString(text.c_str());
			ev.sound = RE::BSFixedString();
			ev.canThrottle = true;
			ev.isWarning = a_isWarning;
			source->Notify(ev);
		}
	}

	void Show(std::string_view a_text)
	{
		Fire(a_text, false);
	}

	void SetDebugEnabled(bool a_on)
	{
		g_debugEnabled.store(a_on, std::memory_order_relaxed);
		REX::DEBUG("[UI] debug notifications {}", a_on ? "ENABLED" : "disabled");
	}

	bool DebugEnabled()
	{
		return g_debugEnabled.load(std::memory_order_relaxed);
	}

	void Debug(std::string_view a_text)
	{
		if (g_debugEnabled.load(std::memory_order_relaxed)) {
			Show(a_text);
		}
	}

	void Error(std::string_view a_text)
	{
		// Always shown — bypasses the debug gate. A player needs to know when something
		// they asked for failed, even with diagnostics off.
		Fire("OSF error: " + std::string{ a_text }, true);
	}
}
