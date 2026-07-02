#pragma once

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
}
