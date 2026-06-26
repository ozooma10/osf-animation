#include "UI/HudMessage.h"

#include <string>

namespace OSF::UI::HudMessage
{
	namespace
	{
		std::atomic_bool g_debugEnabled{ true };
	}

	void Show(std::string_view a_text)
	{
		auto* source = RE::ShowHUDMessageEvent::GetEventSource();
		if (!source) {
			REX::DEBUG("[UI] no ShowHUDMessageEvent source yet — dropped '{}'", a_text);
			return;
		}
		// {text, sound = none, canThrottle = true, isWarning = false} — matches the Papyrus
		// Debug.Notification native (1.16.244). canThrottle lets the HUD coalesce a burst.
		const std::string  text{ a_text };
		RE::ShowHUDMessageEvent ev{};
		ev.text = RE::BSFixedString(text.c_str());
		ev.sound = RE::BSFixedString();
		ev.canThrottle = true;
		ev.isWarning = false;
		source->Notify(ev);
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
}
