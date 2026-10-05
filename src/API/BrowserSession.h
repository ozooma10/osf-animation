#pragma once

#include "API/OSFUI.h"
#include <functional>

namespace OSF::API::BrowserSession
{
	inline constexpr const char* kViewId = "osf.animation/browser";
	OSFUI::API::Client& Views();
	bool Connect(void (*a_shown)(), void (*a_hidden)());
	bool Available();
	const char* UnavailableReason();
	void FinishRegistration();
	bool RegisterSend(const char* a_name, OSFUI::API::SendFn a_handler, bool a_requiresVisible = true);
	bool RegisterPointer(OSFUI::API::RelativePointerFn a_handler);
	void Post(std::function<void()> a_task, bool a_requiresVisible = true);
	void Reset();
}
