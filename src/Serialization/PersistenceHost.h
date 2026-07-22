#pragma once

namespace OSF::Serialization::PersistenceHost
{
	// Installs the verified save/load/delete name hooks, registers OSF Animation
	// as client OSFA, and publishes the API. Call during SFSE_PLUGIN_LOAD.
	bool Initialize();
	bool IsReady();

	// Registers form-remap/delete sinks once event sources exist (kPostDataLoad).
	void RegisterEventSinks();

	// World-load lifecycle entry points used by SaveSafety. BeginLoad is
	// idempotent with the load-name hook; OnLoadBackstop covers new game/Unity.
	void BeginLoad(const char* reason);
	void OnLoadBackstop();
}
