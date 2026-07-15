#pragma once

// Top-right HUD popups (the engine's "notification" corner). Fires the same ShowHUDMessageEvent the Papyrus Debug.Notification native fires 
// the free RE::DebugNotification helper resolves to {0} on 1.16.x and must not be called.

#include <string_view>

namespace OSF::UI::HudMessage
{
	// Fire a HUD popup now. No-op if the event source isn't up yet. Safe from any thread.
	void Show(std::string_view a_text);

	// Opt-in diagnostic popups (OSF UI settings menu, osf "debugNotifications"; default off).
	void SetDebugEnabled(bool a_on);
	bool DebugEnabled();

	// Fire a HUD popup ONLY when debug popups are enabled (callers don't gate themselves).
	void Debug(std::string_view a_text);

	// Fire a HUD popup for a user-facing error. ALWAYS shown, regardless of the debug
	// setting — errors mean "the thing you asked for didn't happen", so the player must
	// see them. Rendered as a warning (isWarning) and prefixed "OSF error: ".
	void Error(std::string_view a_text);
}
