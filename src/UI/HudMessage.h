#pragma once

// Fires ShowHUDMessageEvent like Papyrus Debug.Notification; RE::DebugNotification is {0} on 1.16.x, never call it.

#include <string_view>

namespace OSF::UI::HudMessage
{
	// No-op until the event source is up. Any thread.
	void Show(std::string_view a_text);

	// Without the "OSF error" prefix.
	void Warning(std::string_view a_text);

	// Always shown; prefixed "OSF error: ".
	void Error(std::string_view a_text);
}
