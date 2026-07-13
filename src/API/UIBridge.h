#pragma once

#include <string_view>

// OSF Animation's consumer of the OSF UI native bridge (src/API/OSFUI_API.h).
namespace OSF::API
{
	// Request the OSF UI bridge and register the osf.* commands. A no-op (logged) when OSF UI is absent.
	void InstallUIBridge();

	// Re-push the scene catalog to the view (GAME MAIN THREAD). A no-op until the bridge is ready.
	// Fired when the background clip-duration scan lands new numbers after the initial push.
	void PushCatalogUpdate();

	// True when OSF UI is present and the bridge was fetched (i.e. F10 actually opens something).
	bool UIBridgeInstalled();

	// Open the in-game scene browser (the "osf" view), as F10 would. Backs the OSF.OpenBrowser
	// native so an in-game item (the Data Slate) can surface it. Returns false when OSF UI is
	// absent or too old to support a native menu open (bridge MINOR < 1). Any thread.
	bool OpenBrowser();

	// Open the osf view in emote-wheel mode, filtered to solo scenes whose tags start with
	// a_tagPrefix ("" -> "player.emote."). Captures the crosshair NPC as the wheel's target
	// (dead / in-combat / non-human fall back to a player-only wheel) and delivers
	// osf.mode {mode:"wheel"} race-safely (immediate send + osf.opened replay). Returns false
	// when OSF UI is absent or too old (logs + HUD-errors itself). Any thread.
	bool OpenWheel(std::string_view a_tagPrefix);
}
