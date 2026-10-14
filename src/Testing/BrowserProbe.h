#pragma once
// Private harness uses the same optional DLL boundary as consumers; no addon implementation is linked.
#include "OSFUI.h"
#include "API/BrowserForwarder.h"

namespace OSF::Testing::BrowserProbe
{
	inline constexpr const char* kViewId = "osf.animation/browser";
	inline OSFUI::API::Client& Views() { static OSFUI::API::Client views; if (!views) views.Init(); return views; }
	inline bool Available() { return REX::W32::GetModuleHandleW(L"OSF Animation Browser.dll") && Views(); }
	inline bool Visible()
	{
		const auto module=REX::W32::GetModuleHandleW(L"OSF Animation Browser.dll");
		using Fn=bool (*)();
		const auto fn=module ? reinterpret_cast<Fn>(REX::W32::GetProcAddress(module,"OSFBrowser_IsVisible")) : nullptr;
		return fn && fn();
	}
	inline bool OpenWheel(const char* prefix)
	{
		const auto module=REX::W32::GetModuleHandleW(L"OSF Animation Browser.dll");
		using Fn=bool (*)(const char*);
		const auto fn=module ? reinterpret_cast<Fn>(REX::W32::GetProcAddress(module,"OSFBrowser_OpenWheel")) : nullptr;
		return fn && fn(prefix);
	}
}
