#pragma once

namespace OSF::Serialization::PersistenceHost
{
	// Call during SFSE_PLUGIN_LOAD.
	bool Initialize();

	// kPostDataLoad.
	void RegisterEventSinks();

	// BeginLoad is idempotent with the load-name hook; OnLoadBackstop covers new game.
	// AbortLoad closes a window whose load was refused before dispatch (e.g. F9 with no quicksave);
	// a latched window would make the next real load skip revert.
	void BeginLoad(const char* reason);
	void AbortLoad();
	void OnLoadBackstop();
}
