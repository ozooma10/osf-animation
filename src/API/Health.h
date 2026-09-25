#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

// Reports durable, actionable conditions to OSF Settings Mod Issues (diagnostics ABI 1.0).
// Every entry point is thread-safe. Reports before Connect are buffered; logs remain
// available when OSF Settings is absent. See docs/HEALTH_REPORTING.md.
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

	// Report the scene + sound registries' load problems as one card per failed
	// file, then sweep the files that are now clean. Call after LoadAll() —
	// including the OSF.ReloadScenes path, which is exactly where the sweep
	// earns its keep: fixing a pack and reloading removes its card
	// instead of leaving a stale one behind.
	void ReportRegistryLoad();
}
