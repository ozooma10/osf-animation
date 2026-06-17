#pragma once

namespace OSF::Serialization::SaveSafety
{
	// Registers the SaveLoadEvent "begin" sink, which drops graphs and scenes before a load
	// replaces the world, plus a TESLoadGameEvent backstop that re-binds the natives onto the
	// rebuilt VM and does a late StopAll. Call this at kPostDataLoad.
	void RegisterLoadEventSinks();
}
