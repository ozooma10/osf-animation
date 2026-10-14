#pragma once

// Schema: dist/SFSE/Plugins/OSF/Settings/schemas/osf.animation.engine.json.
namespace OSF::API
{
	// Initialize once at kPostDataLoad after registries load; callbacks may run on SFSE workers.
	void InstallEngineSettings();
}
