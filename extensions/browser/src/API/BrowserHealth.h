#pragma once
#include "OSFSettings_Diagnostics.h"
#include <string_view>
#include <nlohmann/json.hpp>

namespace OSF::API::Health
{
	enum class Severity { kWarning, kError };
	inline OSFSettings::API::Diagnostics::Client client;
	inline void Connect() { client.Init(); }
	inline void Clear(std::string_view id) { if (client) client.Clear("osf.animation.browser",std::string(id).c_str()); }
	inline void Report(std::string_view id, std::string_view code, Severity severity, std::string_view subject, const nlohmann::json* context = nullptr)
	{
		if (!client) return;
		const auto impact = std::string(subject) + (context ? "\n" + context->dump() : "");
		client.Report({.modId="osf.animation.browser",.id=std::string(id).c_str(),
			.severity=severity == Severity::kError ? OSFSettings::API::Diagnostics::Severity::Error : OSFSettings::API::Diagnostics::Severity::Warning,
			.title=code == "wheel.pins-not-persisted" ? "Animation wheel changes could not be saved" : "Animation Browser is unavailable",
			.impact=impact.c_str(),
			.nextSteps=code == "wheel.pins-not-persisted" ? "Check disk space and write permissions, then save the wheel again." : "Reinstall matching Animation Browser and OSF UI releases, then restart Starfield."});
	}
}
