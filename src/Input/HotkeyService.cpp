#include "Input/HotkeyService.h"

#include "API/UIBridge.h"
#include "Input/InputService.h"
#include "Registry/SceneRegistry.h"
#include "Scene/SceneLauncher.h"
#include "Scene/SceneRuntime.h"
#include "UI/HudMessage.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <charconv>

namespace OSF::Input
{
	namespace
	{
		// Immutable after Configure() (kPostDataLoad, before the input hook installs at
		// kPostPostDataLoad), so the hot path reads it without locks.
		std::vector<Hotkey> g_table;

		// VK codes the input hook's director/compat channels already claim (VerbForKeyboard +
		// kActivateKeyVK in InputService.cpp). A hotkey on one would double-fire during a scene.
		bool IsReservedVk(std::uint32_t a_vk)
		{
			switch (a_vk) {
			case 0x20:  // Space -> Advance
			case 0x50:  // P     -> Pause
			case 0x23:  // End   -> End
			case 0x30:  // '0'   -> SpeedReset
			case 0xBB:  // OEM '=' / '+' -> SpeedUp
			case 0x6B:  // Numpad +      -> SpeedUp
			case 0xBD:  // OEM '-'       -> SpeedDown
			case 0x6D:  // Numpad -      -> SpeedDown
			case 0x45:  // E -> SAF-compat activate redirect
				return true;
			default:
				return false;
			}
		}

		// Case-insensitive key name -> Windows VK code ("F10", "g", "Numpad5", "PageUp", or
		// "0x2D"-style hex for anything unlisted). 0 = unrecognized.
		std::uint32_t VkFromName(std::string_view a_name)
		{
			const std::string s = Util::ToLower(a_name);
			if (s.empty()) {
				return 0;
			}
			// A..Z / 0..9: the VK code IS the uppercase ASCII code.
			if (s.size() == 1) {
				const char c = s[0];
				if (c >= 'a' && c <= 'z') {
					return 0x41 + static_cast<std::uint32_t>(c - 'a');
				}
				if (c >= '0' && c <= '9') {
					return 0x30 + static_cast<std::uint32_t>(c - '0');
				}
				return 0;
			}
			// F1..F24 (VK_F1 = 0x70).
			if (s[0] == 'f' && s.size() <= 3) {
				std::uint32_t n = 0;
				bool          digits = true;
				for (std::size_t i = 1; i < s.size(); ++i) {
					if (s[i] < '0' || s[i] > '9') {
						digits = false;
						break;
					}
					n = n * 10 + static_cast<std::uint32_t>(s[i] - '0');
				}
				if (digits && n >= 1 && n <= 24) {
					return 0x70 + n - 1;
				}
			}
			// Numpad0..Numpad9 (VK_NUMPAD0 = 0x60).
			if (s.size() == 7 && s.starts_with("numpad") && s[6] >= '0' && s[6] <= '9') {
				return 0x60 + static_cast<std::uint32_t>(s[6] - '0');
			}
			// Raw hex escape hatch for anything the named set misses.
			if (s.starts_with("0x")) {
				std::uint32_t v = 0;
				const auto    r = std::from_chars(s.data() + 2, s.data() + s.size(), v, 16);
				if (r.ec == std::errc{} && r.ptr == s.data() + s.size() && v >= 1 && v <= 0xFE) {
					return v;
				}
				return 0;
			}
			// Curated named set.
			struct NamedKey
			{
				std::string_view name;
				std::uint32_t    vk;
			};
			static constexpr NamedKey kNamed[] = {
				{ "space", 0x20 }, { "tab", 0x09 }, { "enter", 0x0D }, { "backspace", 0x08 },
				{ "insert", 0x2D }, { "delete", 0x2E }, { "home", 0x24 }, { "end", 0x23 },
				{ "pageup", 0x21 }, { "pagedown", 0x22 },
				{ "up", 0x26 }, { "down", 0x28 }, { "left", 0x25 }, { "right", 0x27 },
				{ "minus", 0xBD }, { "equals", 0xBB }, { "comma", 0xBC }, { "period", 0xBE },
				{ "slash", 0xBF }, { "backslash", 0xDC }, { "semicolon", 0xBA },
				{ "apostrophe", 0xDE }, { "leftbracket", 0xDB }, { "rightbracket", 0xDD },
				{ "grave", 0xC0 },
			};
			for (const auto& k : kNamed) {
				if (s == k.name) {
					return k.vk;
				}
			}
			return 0;
		}

		// Split a pre-lowercased comma-joined tag list ("player.sit,solo"), dropping empties.
		std::vector<std::string> SplitTags(const std::string& a_arg)
		{
			std::vector<std::string> tags;
			std::size_t              start = 0;
			while (start <= a_arg.size()) {
				auto end = a_arg.find(',', start);
				if (end == std::string::npos) {
					end = a_arg.size();
				}
				if (end > start) {
					tags.emplace_back(a_arg.substr(start, end - start));
				}
				start = end + 1;
			}
			return tags;
		}

		// The sit/lean-anywhere toggle: end the player's live scene when it matches the hotkey's
		// tags, else matchmake-and-start one. Game thread.
		void ToggleSceneTags(const std::string& a_arg)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return;
			}
			const auto tags = SplitTags(a_arg);
			if (tags.empty()) {
				return;  // Configure refuses empty args; belt-and-braces
			}

			auto& runtime = Scene::SceneRuntime::GetSingleton();
			if (const auto handle = runtime.GetSceneForActor(player); handle != 0) {
				// Only end a scene this hotkey owns (every hotkey tag on the def's tagSet). A
				// files/ad-hoc scene resolves to no def -> non-matching; never yank someone
				// else's scene.
				const auto  id = runtime.GetId(handle);
				const auto* def = id.empty() ? nullptr : Registry::SceneRegistry::GetSingleton().Find(id);
				const bool  matches = def && std::all_of(tags.begin(), tags.end(),
					 [def](const std::string& t) { return def->tagSet.contains(t); });
				if (matches) {
					REX::DEBUG("[Hotkey] toggleSceneTags '{}': ending scene '{}' (handle {:#010x})", a_arg, id, handle);
					runtime.StopForActor(player);
				} else {
					REX::DEBUG("[Hotkey] toggleSceneTags '{}': player is in non-matching scene '{}' — ignoring", a_arg, id);
				}
				return;
			}

			// IsInCombat is an RE virtual with no other caller in this codebase yet — flagged for
			// in-game verification.
			if (player->IsInCombat()) {
				REX::DEBUG("[Hotkey] toggleSceneTags '{}': player in combat — not starting", a_arg);
				UI::HudMessage::Error("can't start while in combat");
				return;
			}

			Scene::LaunchOpts     opts{};
			Matchmaking::TagQuery query;
			query.allOf = tags;
			Scene::LaunchMatched({ player }, query, opts, Scene::MakeOverrides(opts), "[Hotkey] toggleSceneTags");
		}
	}

	HotkeyService& HotkeyService::GetSingleton()
	{
		static HotkeyService instance;
		return instance;
	}

	void HotkeyService::Configure(const std::vector<std::pair<std::string, std::string>>& a_entries)
	{
		g_table.clear();
		g_table.reserve(a_entries.size());
		for (const auto& [keyName, command] : a_entries) {
			const auto vk = VkFromName(keyName);
			if (vk == 0) {
				REX::ERROR("[Config] hotkeys: unrecognized key name '{}' — skipped (F1-F24, A-Z, 0-9, Numpad0-9, named keys, or \"0x2D\" hex)", keyName);
				continue;
			}
			if (IsReservedVk(vk)) {
				REX::ERROR("[Config] hotkeys: key '{}' is reserved by the scene director channel (Space/P/End/0/+/-/E) — skipped", keyName);
				continue;
			}
			if (std::any_of(g_table.begin(), g_table.end(), [vk](const Hotkey& h) { return h.vk == vk; })) {
				REX::ERROR("[Config] hotkeys: key '{}' bound more than once — keeping the first binding", keyName);
				continue;
			}

			const auto        colon = command.find(':');
			const std::string verb = Util::ToLower(colon == std::string::npos ? std::string_view{ command } : std::string_view{ command }.substr(0, colon));
			const std::string arg = colon == std::string::npos ? std::string{} : command.substr(colon + 1);

			Hotkey hk;
			hk.vk = vk;
			if (verb == "openbrowser") {
				if (!arg.empty()) {
					REX::ERROR("[Config] hotkeys.'{}': openBrowser takes no argument — skipped", keyName);
					continue;
				}
				hk.cmd = HotkeyCommand::kOpenBrowser;
			} else if (verb == "openwheel") {
				hk.cmd = HotkeyCommand::kOpenWheel;
				hk.arg = arg.empty() ? "player.emote." : Util::ToLower(arg);
			} else if (verb == "togglescenetags") {
				if (arg.empty()) {
					REX::ERROR("[Config] hotkeys.'{}': toggleSceneTags needs tags ('toggleSceneTags:tag[,tag..]') — skipped", keyName);
					continue;
				}
				hk.cmd = HotkeyCommand::kToggleSceneTags;
				hk.arg = Util::ToLower(arg);  // matchmaker queries + def tagSets are pre-lowercased
			} else {
				REX::ERROR("[Config] hotkeys.'{}': unknown command '{}' (openBrowser | openWheel[:tagPrefix] | toggleSceneTags:<tags>) — skipped", keyName, command);
				continue;
			}
			g_table.push_back(std::move(hk));
			REX::DEBUG("[Config] hotkey bound: '{}' (VK {:#04x}) -> '{}'", keyName, vk, command);
		}
		InputService::GetSingleton().SetHotkeysArmed(!g_table.empty());
	}

	std::size_t HotkeyService::BindingCount() const
	{
		return g_table.size();
	}

	bool HotkeyService::Matches(std::uint32_t a_vk) const
	{
		for (const auto& hk : g_table) {  // a handful of entries; linear beats a map here
			if (hk.vk == a_vk) {
				return true;
			}
		}
		return false;
	}

	void HotkeyService::Execute(std::uint32_t a_vk)
	{
		const auto it = std::find_if(g_table.begin(), g_table.end(),
			[a_vk](const Hotkey& h) { return h.vk == a_vk; });
		if (it == g_table.end()) {
			return;
		}
		switch (it->cmd) {
		case HotkeyCommand::kOpenBrowser:
			if (!API::OpenBrowser()) {  // reason already logged by OpenBrowser
				UI::HudMessage::Error("OSF UI not present or too old");
			}
			break;
		case HotkeyCommand::kOpenWheel:
			API::OpenWheel(it->arg);  // stub until the wheel UI ships; logs + HUD-errors itself
			break;
		case HotkeyCommand::kToggleSceneTags:
			ToggleSceneTags(it->arg);
			break;
		}
	}
}
