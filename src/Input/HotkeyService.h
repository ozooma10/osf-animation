#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace OSF::Input
{
	// A global-hotkey command verb. openBrowser opens the scene browser; openWheel opens the
	// emote wheel (stub until EmoteWheel_Plan.md lands); toggleSceneTags starts/stops a
	// tag-matched scene for the player.
	enum class HotkeyCommand
	{
		kOpenBrowser,
		kOpenWheel,
		kToggleSceneTags,
	};

	// A parsed hotkey binding: VK code -> command.
	struct Hotkey
	{
		std::uint32_t vk = 0;
		HotkeyCommand cmd = HotkeyCommand::kOpenBrowser;
		std::string   arg;  // openWheel tag prefix / toggleSceneTags comma-joined tags (pre-lowercased)
	};

	// User-configurable global hotkeys (settings.json "hotkeys"), scanned by the input hook on
	// every keyboard press edge. The hook only READS input — a bound key still reaches the game,
	// so users should avoid binding keys the game itself uses heavily. No modifier combos in v1
	// (the raw ButtonEvent carries no modifier state).
	class HotkeyService  // singleton, same shape as InputService
	{
	public:
		static HotkeyService& GetSingleton();

		// Build-time (before InputService::Install): parse "keyName" -> "command[:arg]" entries.
		// Bad key names / unknown commands -> [Config] ERROR + skip (never fatal). Arms the input
		// hook's hotkey scan when >= 1 binding parsed.
		void Configure(const std::vector<std::pair<std::string, std::string>>& a_entries);

		// Bindings that survived Configure (the [Feature] report line).
		std::size_t BindingCount() const;

		// Hot path (input-hook thunk): true if a_vk is bound. Lock-free — the table is immutable
		// after Configure(), which runs before the hook installs.
		bool Matches(std::uint32_t a_vk) const;

		// Game thread (SFSE task): execute the command bound to a_vk.
		void Execute(std::uint32_t a_vk);
	};
}
