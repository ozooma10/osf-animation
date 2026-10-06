#pragma once

// The engine's own subtitle box, driven only by events: ShowSubtitleEvent (AddrLib 86874) / HideSubtitleEvent (86875).
// Renders regardless of subtitle settings; falls back to a HUD message if the source can't resolve.
// Direct Notify bypasses the vanilla auto-hide, so Tick() hides at the deadline and OnStopAll() hides immediately.

#include <string_view>

namespace RE
{
	class Actor;
}

namespace OSF::UI::Subtitle
{
	// a_speaker nullptr = unattributed; a_seconds <= 0 = default hold. Any thread.
	void Show(RE::Actor* a_speaker, std::string_view a_text, float a_seconds = 0.0f);

	void Tick();

	// Synchronous from GraphManager::StopAll.
	void OnStopAll();
#ifdef OSF_ANIMATION_SMOKE
	bool HoldingForSmoke();
#endif
}
