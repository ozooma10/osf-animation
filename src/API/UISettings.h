#pragma once

// Schema: dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.json.
namespace OSF::API
{
	// Initialize once after kPostPostLoad; callbacks may run on SFSE workers.
	void InstallUISettings();
}
