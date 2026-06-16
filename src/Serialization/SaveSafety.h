#pragma once

namespace OSF::Serialization::SaveSafety
{
	// Registers the SaveLoadEvent begin sink (drops graphs/scenes before a  world-replacing load) 
	// and the TESLoadGameEvent backstop (re-binds natives onto the rebuilt VM + a late StopAll). Call at kPostDataLoad.
	void RegisterLoadEventSinks();
}
