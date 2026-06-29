#pragma once

namespace OSF::Papyrus
{
	inline constexpr std::string_view SCRIPT_NAME = "OSF";
	inline constexpr std::string_view ADVANCED_SCRIPT_NAME = "OSFAdvanced";

	// Non-public natives the SAF->OSF compatibility shim needs to reproduce SAF behaviour the OSFdoesn't expose
	inline constexpr std::string_view COMPAT_SCRIPT_NAME = "OSFCompat";
	
	// The NORMAL (non-Native) script that owns the Papyrus structsf. It must stay a normal script: the VM only loads .pex struct types for non-Native scripts.
	// Holds SceneOptions and SceneEvent (the relay marshals into OSFTypes#SceneEvent); force-loaded so both struct types are registered before bind/dispatch.
	inline constexpr std::string_view TYPES_SCRIPT_NAME = "OSFTypes";

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm);
	bool RegisterFunctions();
}
