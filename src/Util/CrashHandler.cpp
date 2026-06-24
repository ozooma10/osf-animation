#include "Util/CrashHandler.h"

// DbgHelp requires <Windows.h> first. The pch already pulls Windows in via
// RE/Starfield.h; we include it explicitly so this TU is self-describing.
#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib")

#include <crtdbg.h>  // _CrtSetReportHook2 etc. (debug CRT / _DEBUG only — guarded below)
#include <csignal>   // std::signal, SIGABRT
#include <cstdarg>
#include <cstdio>
#include <cstdlib>  // _set_abort_behavior, _set_error_mode, _set_invalid_parameter_handler
#include <cstring>
#include <ctime>

#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>

namespace OSF::Util::CrashHandler
{
	namespace
	{
		std::atomic<bool>            g_installUEF{ true };
		std::atomic<bool>            g_installed{ false };
		LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter{ nullptr };  // trainwreck's filter — we chain to it

		// DbgHelp Sym* and MiniDumpWriteDump are documented single-threaded — serialize them.
		std::mutex        g_dbgHelpMutex;
		std::atomic<bool> g_symInitialized{ false };
		// Re-entrancy guard: a fault while reporting a fault must not recurse forever.
		std::atomic<bool> g_inHandler{ false };

		// Cached at Install() time so the dying-process path does no discovery / heavy alloc.
		std::filesystem::path g_logDir;
		std::wstring          g_modulePath;  // full path of our own DLL (PDB search + dump fallback dir)

		std::filesystem::path ResolveLogDir() noexcept
		{
			try {
				if (auto dir = SFSE::log::log_directory()) {
					return *dir;  // ...\Documents\My Games\Starfield\SFSE\Logs
				}
			} catch (...) {}
			try {
				if (!g_modulePath.empty()) {
					return std::filesystem::path(g_modulePath).parent_path();
				}
			} catch (...) {}
			return {};
		}

		std::string PluginName() noexcept
		{
			try {
				return std::string{ SFSE::GetPluginName() };
			} catch (...) {
				return "OSF Animation";
			}
		}

		std::string TimeStamp() noexcept
		{
			const std::time_t t = std::time(nullptr);
			std::tm           tm{};
			localtime_s(&tm, &t);
			char buf[32]{};
			std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
			return buf;
		}

		// Allocation-light logging for the crash context. We write to a DEDICATED crash log
		// (a fresh FILE* each call) rather than routing through spdlog: in an abort context the
		// heap or the CRT lock may be compromised and spdlog allocates/formats. We also mirror
		// to the debugger via OutputDebugStringA. Do NOT "improve" this to use REX/spdlog.
		void RawLog(const char* a_line) noexcept
		{
			OutputDebugStringA(a_line);
			OutputDebugStringA("\n");
			try {
				const auto path = g_logDir / std::format("{}_crash.log", PluginName());
				std::FILE* f = nullptr;
				if (_wfopen_s(&f, path.wstring().c_str(), L"a") == 0 && f) {
					std::fprintf(f, "%s\n", a_line);
					std::fclose(f);
				}
			} catch (...) {}
		}

		void RawLogf(const char* a_fmt, ...) noexcept
		{
			char    buf[1024]{};
			va_list args;
			va_start(args, a_fmt);
			std::vsnprintf(buf, sizeof(buf), a_fmt, args);
			va_end(args);
			RawLog(buf);
		}

		void EnsureSymInit() noexcept
		{
			if (g_symInitialized.load(std::memory_order_acquire)) {
				return;
			}
			SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
			std::string searchPath;
			try {
				if (!g_modulePath.empty()) {
					searchPath = std::filesystem::path(g_modulePath).parent_path().string();
				}
			} catch (...) {}
			if (SymInitialize(GetCurrentProcess(), searchPath.empty() ? nullptr : searchPath.c_str(), TRUE)) {
				g_symInitialized.store(true, std::memory_order_release);
			}
		}

		// Symbolize one return address; falls back to "Module+0xRVA" (still resolvable offline
		// against the shipped PDB) when no symbol/line is available.
		void SymbolizeFrame(void* a_addr, char* a_out, size_t a_outSize) noexcept
		{
			const auto proc = GetCurrentProcess();
			const auto addr = reinterpret_cast<DWORD64>(a_addr);

			char    moduleName[MAX_PATH] = "???";
			DWORD64 moduleBase           = 0;
			HMODULE mod                  = nullptr;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(a_addr), &mod) &&
				mod) {
				moduleBase = reinterpret_cast<DWORD64>(mod);
				char full[MAX_PATH]{};
				if (GetModuleFileNameA(mod, full, MAX_PATH)) {
					const char* base = std::strrchr(full, '\\');
					std::snprintf(moduleName, sizeof(moduleName), "%s", base ? base + 1 : full);
				}
			}
			const DWORD64 rva = moduleBase ? (addr - moduleBase) : 0;

			alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512]{};
			auto* sym         = reinterpret_cast<SYMBOL_INFO*>(symBuf);
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen   = 512;

			DWORD64    symDisp = 0;
			const bool haveSym = SymFromAddr(proc, addr, &symDisp, sym) != FALSE;

			IMAGEHLP_LINE64 line{};
			line.SizeOfStruct = sizeof(line);
			DWORD      lineDisp = 0;
			const bool haveLine = SymGetLineFromAddr64(proc, addr, &lineDisp, &line) != FALSE;

			if (haveSym && haveLine) {
				std::snprintf(a_out, a_outSize, "%s!%s+0x%llx  [%s:%lu]  (%s+0x%llx)",
					moduleName, sym->Name, static_cast<unsigned long long>(symDisp),
					line.FileName, line.LineNumber, moduleName, static_cast<unsigned long long>(rva));
			} else if (haveSym) {
				std::snprintf(a_out, a_outSize, "%s!%s+0x%llx  (%s+0x%llx)",
					moduleName, sym->Name, static_cast<unsigned long long>(symDisp),
					moduleName, static_cast<unsigned long long>(rva));
			} else {
				std::snprintf(a_out, a_outSize, "%s+0x%llx", moduleName, static_cast<unsigned long long>(rva));
			}
		}

		void LogBacktrace() noexcept
		{
			void*        frames[62]{};
			const USHORT n = RtlCaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)), frames, nullptr);

			std::scoped_lock lock(g_dbgHelpMutex);
			EnsureSymInit();
			RawLog("---- stack backtrace ----");
			for (USHORT i = 0; i < n; ++i) {
				char frameStr[1024]{};
				SymbolizeFrame(frames[i], frameStr, sizeof(frameStr));
				RawLogf("  [%2u] %p  %s", i, frames[i], frameStr);
			}
			RawLog("-------------------------");
		}

		// a_ep is null on the assert/abort path (no EXCEPTION_POINTERS); non-null on the SEH path.
		void WriteMiniDump(EXCEPTION_POINTERS* a_ep) noexcept
		{
			std::filesystem::path dmpPath;
			try {
				dmpPath = g_logDir / std::format("{}_crash_{}.dmp", PluginName(), TimeStamp());
			} catch (...) {
				return;
			}

			HANDLE hFile = CreateFileW(dmpPath.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (hFile == INVALID_HANDLE_VALUE) {
				RawLogf("CrashHandler: could not create dump (err %lu)", GetLastError());
				return;
			}

			MINIDUMP_EXCEPTION_INFORMATION  mei{};
			MINIDUMP_EXCEPTION_INFORMATION* meiPtr = nullptr;
			if (a_ep) {
				mei.ThreadId          = GetCurrentThreadId();
				mei.ExceptionPointers = a_ep;
				mei.ClientPointers    = FALSE;
				meiPtr                = &mei;
			}
			const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo);

			BOOL ok;
			{
				std::scoped_lock lock(g_dbgHelpMutex);
				ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, type, meiPtr, nullptr, nullptr);
			}
			CloseHandle(hFile);
			if (ok) {
				RawLogf("CrashHandler: minidump -> %s", dmpPath.string().c_str());
			} else {
				RawLogf("CrashHandler: MiniDumpWriteDump failed (err %lu)", GetLastError());
			}
		}

#ifdef _DEBUG
		// Debug-CRT (/MDd) only: assert() routes through _CrtDbgReport, which this hook intercepts
		// BEFORE the modal dialog. Under the release CRT (/MD) this is never reached — assert() goes
		// straight to _wassert -> abort(), which the SIGABRT handler below catches instead.
		int __cdecl ReportHook(int a_reportType, char* a_message, int* a_returnValue) noexcept
		{
			if (a_reportType != _CRT_ASSERT && a_reportType != _CRT_ERROR) {
				return FALSE;  // let _CRT_WARN flow normally
			}
			if (!g_inHandler.exchange(true)) {
				RawLog("==================== CRT ASSERT ====================");
				RawLogf("%s", a_message ? a_message : "(no message)");  // _wassert text: expr / file / line
				LogBacktrace();
				WriteMiniDump(nullptr);
				RawLog("===================================================");
				g_inHandler.store(false);
			}
			if (a_returnValue) {
				*a_returnValue = 0;  // do not break into the debugger
			}
			return TRUE;  // handled: suppress the Abort/Retry/Ignore dialog (CRT still abort()s)
		}
#endif

		// Release-CRT edge: a CRT function detected a bad argument. expr/func/file may be empty in release.
		void __cdecl InvalidParameterHandler(const wchar_t* a_expr, const wchar_t* a_func,
			const wchar_t* a_file, unsigned int a_line, uintptr_t) noexcept
		{
			if (!g_inHandler.exchange(true)) {
				char expr[256]{}, func[256]{}, file[512]{};
				std::snprintf(expr, sizeof(expr), "%ls", a_expr ? a_expr : L"(n/a)");
				std::snprintf(func, sizeof(func), "%ls", a_func ? a_func : L"(n/a)");
				std::snprintf(file, sizeof(file), "%ls", a_file ? a_file : L"(n/a)");
				RawLog("================ CRT INVALID PARAMETER ================");
				RawLogf("expr=%s func=%s file=%s line=%u", expr, func, file, a_line);
				LogBacktrace();
				WriteMiniDump(nullptr);
				RawLog("======================================================");
				g_inHandler.store(false);
			}
			// fall through: the CRT proceeds to abort() -> AbortHandler also fires (re-entrancy-guarded)
		}

		// The path that catches the BSFixedString.h:53 assert in this (/MD, asserts-on) build:
		// release-CRT assert() -> _wassert -> abort() -> raise(SIGABRT).
		void __cdecl AbortHandler(int) noexcept
		{
			if (!g_inHandler.exchange(true)) {
				RawLog("======================== SIGABRT (assert/abort) ========================");
				LogBacktrace();
				WriteMiniDump(nullptr);
				RawLog("=======================================================================");
				// leave g_inHandler set — the process is about to terminate
			}
		}

		LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* a_ep) noexcept
		{
			if (!g_inHandler.exchange(true)) {
				RawLogf("==================== UNHANDLED SEH (code 0x%08lX) ====================",
					a_ep && a_ep->ExceptionRecord ? a_ep->ExceptionRecord->ExceptionCode : 0UL);
				LogBacktrace();
				WriteMiniDump(a_ep);
				RawLog("=====================================================================");
				g_inHandler.store(false);
			}
			// Chain to trainwreck (or default) so its crash log still gets produced.
			return g_prevFilter ? g_prevFilter(a_ep) : EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void SetInstallUnhandledExceptionFilter(bool a_enable) noexcept
	{
		g_installUEF.store(a_enable, std::memory_order_relaxed);
	}

	void Install() noexcept
	{
		if (g_installed.exchange(true)) {
			return;  // idempotent
		}

		// Cache our own module path (PDB search + dump fallback dir) and the log directory.
		if (HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&Install), &self) &&
			self) {
			wchar_t buf[MAX_PATH]{};
			if (GetModuleFileNameW(self, buf, MAX_PATH)) {
				g_modulePath = buf;
			}
		}
		g_logDir = ResolveLogDir();

#ifdef _DEBUG
		// Debug-CRT path: intercept asserts before the dialog and also keep a stderr sink.
		_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, ReportHook);
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif

		// Release-CRT (and generally): keep abort() from blocking on a modal box / WER, and
		// catch the invalid-parameter and SIGABRT paths so we still get a stack + dump.
		_set_error_mode(_OUT_TO_STDERR);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
		_set_invalid_parameter_handler(InvalidParameterHandler);
		std::signal(SIGABRT, AbortHandler);

		if (g_installUEF.load(std::memory_order_relaxed)) {
			g_prevFilter = SetUnhandledExceptionFilter(UnhandledFilter);
		}

		RawLogf("CrashHandler installed (log dir: %s)", g_logDir.string().c_str());
	}
}
