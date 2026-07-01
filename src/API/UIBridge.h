#pragma once

// OSF Animation's consumer of the OSF UI native bridge (src/API/OSFUI_API.h).
// It registers the osf.* scene-browser commands and answers them IN-PROCESS
// (reading its own live scene registry, resolving the crosshair, and launching
// through its own scene API). Only JSON text crosses the OSF UI boundary; no
// RE::*/STL types ever do. See docs/native-plugin-api.md in the OSF UI repo.
namespace OSF::API
{
	// Request the OSF UI bridge and register the osf.* commands. A no-op (logged)
	// when OSF UI is absent. Call once from SFSE kPostDataLoad, AFTER the scene
	// registry is loaded and the native scene API has been marked ready.
	void InstallUIBridge();
}
