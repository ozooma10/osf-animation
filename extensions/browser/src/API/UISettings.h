#pragma once

// Browser owns saved preferences; OSF Settings owns hotkeys and the shared engine log level.
namespace OSF::API
{
	// Initialize once at kPostPostDataLoad; callbacks may run on SFSE workers.
	void InstallUISettings();
}
