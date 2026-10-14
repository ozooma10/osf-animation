#pragma once

// Schema: dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.browser.json.
namespace OSF::API
{
	// Initialize once after kPostPostLoad; callbacks may run on SFSE workers.
	void InstallUISettings();
}
