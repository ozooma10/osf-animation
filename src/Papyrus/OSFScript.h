#pragma once

namespace OSF::Papyrus
{
	inline constexpr std::string_view SCRIPT_NAME = "OSF";

	void RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm);
	bool RegisterFunctions();
}
