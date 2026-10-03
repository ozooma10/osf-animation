#pragma once

namespace OSF::Serialization::SaveSafety
{
	// Call at kPostDataLoad. Without SaveLoadEvent only the TESLoadGameEvent backstop runs (no save window).
	void RegisterLoadEventSinks();
}
