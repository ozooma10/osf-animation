#pragma once

#include <string_view>

namespace OSF::API
{
	void InstallUIBridge();

	// Game main thread; no-op while no OSF UI document is live.
	void PushCatalogUpdate();

	// Any thread; queues the open for the frame update. False when OSF UI is absent or unsupported.
	bool OpenBrowser();

	// Any thread. a_tagPrefix "" -> "player.emote.". Dead/in-combat/non-human crosshair NPCs
	// fall back to a player-only wheel.
	bool OpenWheel(std::string_view a_tagPrefix);

#ifdef OSF_ANIMATION_SMOKE
	// Frame-thread observation after the native lifecycle task has executed.
	bool BrowserVisibleForSmoke();
#endif
}
