#pragma once

// Catches CRT assert/abort and invalid-parameter failures, which trainwreck (SEH only) misses.
// Writes "<SFSE Logs>/<PluginName>_crash.log" (reason + backtrace) and a timestamped minidump.

namespace OSF::Util::CrashHandler
{
	// Call at the top of SFSE_PLUGIN_PRELOAD. Idempotent. Chains to the previous SEH filter.
	void Install() noexcept;
}
