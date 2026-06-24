#pragma once

// Windows/x64/MSVC only. Crash + assertion logging for the OSF Animation SFSE plugin.
//
// Catches the failure classes that trainwreck.dll (which only hooks SEH/access
// violations) misses — in particular a failed CRT assert(), which goes
//   _wassert -> (MessageBox) -> abort() -> raise(SIGABRT)
// and is therefore NOT a structured exception trainwreck can see. That is exactly
// how the BSFixedString.h:53 assertion presented: a modal dialog and no dump.
//
// Every captured failure writes, to "<SFSE Logs>/<PluginName>_crash.log":
//   * a one-line reason (assert expression/file/line where the CRT gives it to us),
//   * a symbolized (or Module+0xRVA) backtrace of the failing thread,
// and a timestamped minidump "<SFSE Logs>/<PluginName>_crash_<ts>.dmp".
//
// See CrashHandler.cpp for the per-build-mode behavior (debug /MDd vs release /MD).

namespace OSF::Util::CrashHandler
{
	// Install all handlers. Call ONCE, as early as possible — top of SFSE_PLUGIN_PRELOAD,
	// before anything (ours or commonlib's) can assert. Idempotent; never throws.
	void Install() noexcept;

	// Set false BEFORE Install() to skip the SetUnhandledExceptionFilter backstop and
	// leave all SEH/access-violation handling to trainwreck. Default true; when enabled
	// we always chain to the previously installed filter so trainwreck still runs.
	void SetInstallUnhandledExceptionFilter(bool a_enable) noexcept;
}
