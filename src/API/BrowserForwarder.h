#pragma once
#include "REX/W32/KERNEL32.h"

namespace OSF::API
{
	// Compatibility only: no startup registration, UI dependency or browser-owned state.
	inline bool OpenBrowser()
	{
		const auto module=REX::W32::GetModuleHandleW(L"OSF Animation Browser.dll");
		using Open=bool (*)();
		const auto open=module ? reinterpret_cast<Open>(REX::W32::GetProcAddress(module,"OSFBrowser_Open")) : nullptr;
		return open && open();
	}
}
