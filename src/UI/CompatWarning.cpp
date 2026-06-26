#include "UI/CompatWarning.h"

#include <REX/W32/KERNEL32.h>
#include <REX/W32/USER32.h>

#include <array>
#include <format>
#include <string>

namespace OSF::UI
{
	namespace
	{
		// Incompatible rig-stamping frameworks, by their shipped SFSE plugin DLL name. 
		// GetModuleHandle is enough: an SFSE plugin is loaded into our process, so a non-null handle means it co-loaded this session.
		struct IncompatibleFramework
		{
			const wchar_t* module;       // DLL as it appears in SFSE/Plugins
			const char*    displayName;  // human-facing name for the popup/log
		};

		constexpr std::array kIncompatibleFrameworks{
			IncompatibleFramework{ L"StarfieldAnimationFramework.dll", "SAF (Starfield Animation Framework)" },
			IncompatibleFramework{ L"NAF.dll", "NAFSF" },
		};

		// Win32 MessageBox type: MB_OK | MB_ICONWARNING | MB_SETFOREGROUND. 
		constexpr std::uint32_t kMessageBoxFlags = 0x00000000 | 0x00000030 | 0x00010000;
	}

	void CompatWarning::ProbeIncompatibilities()
	{
		std::string detected;
		for (const auto& framework : kIncompatibleFrameworks) {
			if (REX::W32::GetModuleHandleW(framework.module) != nullptr) {
				REX::WARN("[UI] co-loaded incompatible animation framework: {}. Both write the engine rig buffer every frame and will fight over poses.", framework.displayName);
				if (!detected.empty()) {
					detected += " and ";
				}
				detected += framework.displayName;
			}
		}

		if (detected.empty()) {
			REX::DEBUG("[UI] no incompatible frameworks detected.");
			return;
		}

		const std::string body = std::format(
			"{} is also installed.\n\n"
			"OSF Animation and {} both drive the same animation system and will fight over actor poses (twitching / T-posing)."
			"They cannot run together.\n\n"
			"To use OSF Animation, DISABLE {} in your mod manager, then restart the game. "
			"(If you would rather keep {}, disable OSF Animation instead.)",
			detected, detected, detected, detected);

		// Blocking Win32 box on the load thread — the game hasn't reached the title  screen yet, 
		// so blocking here is harmless and guarantees the user sees it before any scene plays. nullptr owner = top-level (no game window yet).
		REX::DEBUG("[UI] posting co-load compatibility warning via Win32 MessageBox: {}", detected);
		REX::W32::MessageBoxA(nullptr, body.c_str(), "OSF Animation — conflicting framework detected", kMessageBoxFlags);
	}
}
