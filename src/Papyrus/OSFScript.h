#pragma once

namespace OSF::Papyrus
{
	inline constexpr std::string_view SCRIPT_NAME = "OSF";
	
	// The NORMAL (non-Native) script that owns the Papyrus structsf. It must stay a normal script: the VM only loads .pex struct types for non-Native scripts
	inline constexpr std::string_view TYPES_SCRIPT_NAME = "OSFTypes";

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm);
	bool RegisterFunctions();
}
