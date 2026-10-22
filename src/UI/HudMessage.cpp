#include "UI/HudMessage.h"

#include <string>

namespace OSF::UI::HudMessage
{
	namespace
	{
		// a_isWarning selects the HUD warning styling.
		void Fire(std::string_view a_text, bool a_isWarning)
		{
			auto* source = RE::ShowHUDMessageEvent::GetEventSource();
			if (!source) {
				REX::DEBUG("[UI] no ShowHUDMessageEvent source yet — dropped '{}'", a_text);
				return;
			}
			// {text, sound = none, canThrottle = true, isWarning}, as Papyrus Debug.Notification.
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

	void Warning(std::string_view a_text)
	{
		Fire(a_text, true);
	}

	void Error(std::string_view a_text)
	{
		Fire("OSF error: " + std::string{ a_text }, true);
	}
}
