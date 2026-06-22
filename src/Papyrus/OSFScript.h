#pragma once

namespace OSF::Papyrus
{
	inline constexpr std::string_view SCRIPT_NAME = "OSF";

	// Separate script name for non-public natives: the crosshair pickers and debug/test probes the OSFTest harness drives.
	// These are deliberately kept off the OSF public surface; the scene runtime drives its own control/camera policy through actions instead. See OSFCompat.psc.
	inline constexpr std::string_view COMPAT_SCRIPT_NAME = "OSFCompat";

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm);
	
	// Convenience overload: grabs the current GameVM and (re)binds every native, returning false if the VM is unavailable.
	// Call this at kPostDataLoad AND after every save load: Starfield tears down and rebuilds the Papyrus VM on load (
	bool RegisterFunctions();
}
