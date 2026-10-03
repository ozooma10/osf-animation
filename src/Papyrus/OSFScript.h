#pragma once

namespace OSF::Papyrus
{
	inline constexpr std::string_view SCRIPT_NAME = "OSF";
	inline constexpr std::string_view ADVANCED_SCRIPT_NAME = "OSFAdvanced";

	// Must stay non-Native: the VM only loads .pex struct types for non-Native scripts.
	// Holds SceneOptions + SceneEvent; force-loaded so both are registered before bind/dispatch.
	inline constexpr std::string_view TYPES_SCRIPT_NAME = "OSFTypes";

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm);
	bool RegisterFunctions();
}
