#include "API/UISettings.h"

#include "API/OSFUI_API.h"
#include "API/UIBridge.h"
#include "UI/FirstRunHint.h"
#include "UI/HudMessage.h"
#include "Util/StringUtil.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace OSF::API
{
	namespace
	{
		// The MCM schema — the same document a settings/osf.json drop-in would
		// hold (docs/schema/settings-schema.schema.json in the OSF UI repo).
		// id "osf" matches the view folder and the osf.* command namespace
		// (action buttons and per-view write scoping key off the mod id).
		// Hotkeys default UNBOUND ("" + allowUnbound): binding is the user's
		// opt-in, exactly like the old empty hotkeys map — but now in-game,
		// rebindable, and conflict-badged.
		constexpr const char* kSchemaJson = R"json({
  "id": "osf",
  "title": "OSF Animation",
  "description": "Scene framework — browser, emote wheel, and scene hotkeys.",
  "icon": "osf-icon.svg",
  "version": 1,
  "groups": [
    { "label": "Hotkeys", "settings": [
      { "key": "hotkeys.openBrowser", "type": "key", "default": "", "allowUnbound": true,
        "label": "Open scene browser",
        "hint": "Opens the scene browser." },
      { "key": "hotkeys.openWheel", "type": "key", "default": "", "allowUnbound": true,
        "label": "Open emote wheel",
        "hint": "Radial emote picker; targets the crosshair NPC when one is in reach." }
    ] },
    { "label": "Interface", "settings": [
      { "key": "debugNotifications", "type": "bool", "default": false,
        "label": "Stage-transition popups",
        "hint": "Debug HUD popup on each scene stage transition." },
      { "key": "firstRunHint", "type": "bool", "default": true,
        "label": "First-run hint",
        "hint": "Show the browser hint until the scene browser has been opened a few times." }
    ] },
    { "label": "Logging", "settings": [
      { "key": "logLevel", "type": "enum", "default": "info",
        "options": ["trace", "debug", "info", "warn", "error"],
        "optionLabels": ["Trace", "Debug", "Info", "Warnings", "Errors"],
        "label": "Log level", "hint": "OSF Animation.log verbosity." }
    ] }
  ]
})json";

		// Fetched once at install (the export returns OSF UI's singleton — the
		// same object UIBridge caches). nullptr => OSF UI absent.
		OSFUI::API::IOSFUIBridge* g_bridge = nullptr;

		// "trace" | "debug" | "info" | "warn" | "error" -> spdlog level, applied
		// to the live logger (moved here from the retired Config::Settings).
		void SetLogLevel(std::string_view a_level)
		{
			const std::string s = Util::ToLower(a_level);
			spdlog::level::level_enum lvl = spdlog::level::info;
			if (s == "trace") {
				lvl = spdlog::level::trace;
			} else if (s == "debug") {
				lvl = spdlog::level::debug;
			} else if (s == "info") {
				lvl = spdlog::level::info;
			} else if (s == "warn" || s == "warning") {
				lvl = spdlog::level::warn;
			} else if (s == "error" || s == "err") {
				lvl = spdlog::level::err;
			} else {
				REX::ERROR("[Config] logLevel '{}' unrecognized — keeping current", a_level);
				return;
			}
			if (const auto logger = spdlog::default_logger()) {
				logger->set_level(lvl);
				logger->flush_on(lvl);
			}
			REX::INFO("[Config] log level set to '{}'", s);
		}

		// SubscribeSettings sink — replayed once per current value at
		// subscribe, then fired on every commit. Game main thread.
		void OnSetting(const char* /*a_modId*/, const char* a_key, const char* a_valueJson, void*) noexcept
		{
			const std::string_view key{ a_key ? a_key : "" };
			const std::string_view value{ a_valueJson ? a_valueJson : "" };
			// Values arrive as serialized JSON ("true", "\"info\"") — strip the
			// quotes for the string-shaped ones.
			const auto unquote = [](std::string_view v) -> std::string {
				if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
					return std::string{ v.substr(1, v.size() - 2) };
				}
				return std::string{ v };
			};
			if (key == "logLevel") {
				SetLogLevel(unquote(value));
			} else if (key == "debugNotifications") {
				UI::HudMessage::SetDebugEnabled(value == "true");
			} else if (key == "firstRunHint") {
				UI::FirstRunHint::SetEnabled(value == "true");
			}
		}

		// SubscribeHotkey sinks — the press already passed OSF UI's gates
		// (console closed, no capture, no rebind armed). Game main thread.
		void OnHotkey(const char* /*a_modId*/, const char* a_key, void*) noexcept
		{
			const std::string_view key{ a_key ? a_key : "" };
			if (key == "hotkeys.openBrowser") {
				OpenBrowser();
			} else if (key == "hotkeys.openWheel") {
				OpenWheel("");  // "" -> the default player.emote. prefix
			}
		}

		// One-time notice for upgraders: the legacy file is dead, and silently
		// ignoring it would look like lost settings.
		void WarnLegacyFile()
		{
			std::error_code ec;
			const auto      file = std::filesystem::current_path() / "Data" / "OSF" / "settings.json";
			if (std::filesystem::is_regular_file(file, ec)) {
				REX::WARN("[Config] Data/OSF/settings.json is NO LONGER READ — settings and hotkeys moved "
				          "to the in-game OSF UI settings menu (delete the file to silence this)");
			}
		}
	}

	void InstallUISettings()
	{
		using namespace OSFUI::API;

		WarnLegacyFile();

		g_bridge = RequestBridge();
		if (!g_bridge) {
			REX::INFO("[Config] OSF UI not present — settings menu + hotkeys unavailable, defaults in effect");
			return;
		}

		const auto minor = g_bridge->GetInterfaceVersion() & 0xFFFFu;
		if (minor < 2u) {
			REX::WARN("[Config] installed OSF UI has no settings surface (bridge MINOR {} < 2) — "
			          "update OSF UI for the settings menu; defaults in effect", minor);
			g_bridge = nullptr;
			return;
		}

		if (!g_bridge->RegisterSettingsSchema(kSchemaJson)) {
			REX::ERROR("[Config] OSF UI rejected the settings schema — defaults in effect");
			g_bridge = nullptr;
			return;
		}
		g_bridge->SubscribeSettings("osf", &OnSetting, nullptr);

		if (minor >= 4u) {
			g_bridge->SubscribeHotkey("osf", "hotkeys.openBrowser", &OnHotkey, nullptr);
			g_bridge->SubscribeHotkey("osf", "hotkeys.openWheel", &OnHotkey, nullptr);
			REX::INFO("[Feature] MCM settings CONNECTED (schema 'osf' registered, 2 hotkeys subscribed)");
		} else {
			REX::WARN("[Feature] MCM settings CONNECTED, but hotkey dispatch needs OSF UI bridge MINOR >= 4 (have {}) — hotkeys inert", minor);
		}
	}
}
