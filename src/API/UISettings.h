#pragma once

// Settings schema: dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.json.
// OSF Settings owns persistence and hotkeys. Animation forwards only browser preferences
// through OSF UI retained state and a scoped write endpoint.
namespace OSF::API
{
	// Initialize once after kPostPostLoad; callbacks may run on SFSE workers.
	void InstallUISettings();
}
