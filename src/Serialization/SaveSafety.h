#pragma once

// Save-safety for the core playback state: on a world-replacing load the engine
// resets every actor to the saved state, so our graphs/scenes — anchored in the
// world that was just discarded — must be dropped (GraphManager::StopAll). This
// also re-binds the Papyrus natives onto the rebuilt VM. Cross-restart aftermath
// PERSISTENCE (cosave: redress/movement-revert of actors saved mid-scene) is an
// OSF Intimacy concern and lives there, not in the core.

namespace OSF::Serialization::SaveSafety
{
	// Registers the SaveLoadEvent begin sink (drops graphs/scenes before a
	// world-replacing load) and the TESLoadGameEvent backstop (re-binds natives
	// onto the rebuilt VM + a late StopAll). Call at kPostDataLoad.
	void RegisterLoadEventSinks();
}
