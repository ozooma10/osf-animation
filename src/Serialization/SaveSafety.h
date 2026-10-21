#pragma once

namespace OSF::Serialization::SaveSafety
{
	// Call at kPostDataLoad. Installs the requested-save dispatch hook and save/load event sinks.
	void RegisterLoadEventSinks();
}
