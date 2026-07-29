#pragma once

namespace OSF::Serialization::PersistenceHost
{
	// Installs the verified save/load/delete name hooks, registers OSF Animation
	// as client OSFA, and publishes the API. Call during SFSE_PLUGIN_LOAD.
	bool Initialize();

	// Registers form-remap/delete sinks once event sources exist (kPostDataLoad).
	void RegisterEventSinks();

	// World-load lifecycle entry points used by SaveSafety. BeginLoad is
	// idempotent with the load-name hook; OnLoadBackstop covers new game/Unity.
	// AbortLoad closes a revert window whose load was refused before it ever
	// dispatched (e.g. F9 with no quicksave) — without it the latched window
	// makes the NEXT real load skip revert/teardown entirely. Gated on the
	// dispatch flag so it never closes a genuinely in-flight load.
	void BeginLoad(const char* reason);
	void AbortLoad();
	void OnLoadBackstop();
}
