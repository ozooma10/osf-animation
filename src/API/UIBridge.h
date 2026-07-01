#pragma once

// OSF Animation's consumer of the OSF UI native bridge (src/API/OSFUI_API.h).
namespace OSF::API
{
	// Request the OSF UI bridge and register the osf.* commands. A no-op (logged) when OSF UI is absent. 
	void InstallUIBridge();
}
