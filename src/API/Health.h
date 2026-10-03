#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

// Reports actionable conditions to OSF Settings Mod Issues. Thread-safe; reports before Connect are buffered.
namespace OSF::API::Health
{
	enum class Severity
	{
		kWarning,  // degraded, still usable
		kError,    // this part does not work
	};

	// Connect after kPostPostLoad and before registry loading.
	void Connect();

	// Raise or replace a condition. Codes select Animation-owned wording.
	// Context is flattened into the impact text; use bare filenames and sanitized details.
	void Report(std::string_view a_id, std::string_view a_code, Severity a_severity,
		std::string_view a_subject = "", const nlohmann::json* a_context = nullptr);

	// Remove a resolved condition. Safe for IDs that were never reported.
	void Clear(std::string_view a_id);

	// One card per failed file; clears cards of files now clean. Call after every LoadAll(), including reloads.
	void ReportRegistryLoad();
}
