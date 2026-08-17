#include "API/UISettings.h"

#include "API/OSFUI_API.h"
#include "API/UIBridge.h"
#include "Equipment/GearRegistry.h"
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
		// The MCM schema — the same document a settings/osf.animation.json
		// drop-in would hold (docs/schema/settings-schema.schema.json in the
		// OSF UI repo). id "osf.animation" (item-1 grammar: author "osf",
		// modname "animation") matches the views/osf.animation/ namespace
		// folder and the osf.animation.* command namespace. NOTE the rename
		// from the pre-1.0 id "osf": old osf.json values files are orphaned
		// (hotkeys default unbound anyway; users rebind once).
		// The hotkey defaults UNBOUND ("" + allowUnbound). F10 (the OSF UI console
		// toggle) already opens the browser, while every plausible gameplay key
		// is context-sensitive and may also be localized. In particular, B is
		// used by several German UI actions. A default must not take ownership
		// of a key that Starfield needs; users can opt in from the settings card.
		// Pages (OSF UI 1.5.0): the Hotkeys group stays untagged so it lands on
		// the implicit General tab painted first; everything else segments into
		// Browser / Scenes / Advanced. Older OSF UI hosts ignore `pages`/`page`
		// and render the same groups as one flat column.
		constexpr const char* kSchemaJson = R"json({
  "id": "osf.animation",
  "title": "OSF Animation",
  "description": "Scene framework — animation browser, scene controls, and hotkeys.",
  "icon": "browser/osf-icon.svg",
  "version": 6,
  "targetVersion": "1.5.0",
  "pages": [
    { "id": "browser", "label": "Browser" },
    { "id": "scenes", "label": "Scenes" },
    { "id": "advanced", "label": "Advanced" }
  ],
  "groups": [
    { "id": "hotkeys", "label": "Hotkeys", "settings": [
      { "key": "hotkeys.openBrowser", "type": "key", "default": "", "allowUnbound": true,
        "label": "Open animation browser",
        "hint": "Browse animations, emotes, and authored scenes." }
    ] },
    { "id": "browser-behavior", "label": "Behavior", "page": "browser", "settings": [
      { "key": "browser.afterLaunch", "type": "enum", "default": "stay",
        "options": ["minimize", "stay", "close"],
        "optionLabels": ["Live controls", "Stay open", "Close browser"],
        "label": "After launching a scene",
        "hint": "Choose what the animation browser does after a successful launch." },
      { "key": "browser.openTo", "type": "enum", "default": "last",
        "options": ["last", "scenes", "active"],
        "optionLabels": ["Last used", "Browse", "Active"],
        "label": "Open browser to",
        "hint": "Active falls back to Browse when nothing is running." },
      { "key": "browser.rememberBrowsing", "type": "bool", "default": true,
        "label": "Remember browsing state",
        "hint": "Keep search, filters, and expanded folders between browser openings in this game session." }
    ] },
    { "id": "browser-library", "label": "Library", "page": "browser", "settings": [
      { "key": "browser.libraryDetail", "type": "enum", "default": "curated",
        "options": ["curated", "full"], "optionLabels": ["Poses and loops", "Full library"],
        "label": "Animation detail",
        "hint": "Choose whether transitions and animation layers are shown by default." },
      { "key": "browser.librarySource", "type": "enum", "default": "all",
        "options": ["all", "custom"], "optionLabels": ["Custom and vanilla", "Custom only"],
        "label": "Animation source",
        "hint": "Hide vanilla animations without affecting custom animation packs." },
      { "key": "browser.unavailableScenes", "type": "enum", "default": "ask",
        "options": ["ask", "show", "hide"], "optionLabels": ["On request", "Always below", "Hide"],
        "label": "Unavailable scenes",
        "hint": "Control scenes that need a different cast or furniture." }
    ] },
    { "id": "browser-display", "label": "Display", "page": "browser", "settings": [
      { "key": "browser.actorLabels", "type": "bool", "default": true,
        "label": "World selection labels",
        "hint": "Tag selected cast members and furniture in the world while the browser is open." },
      { "key": "browser.authorDetails", "type": "bool", "default": false,
        "label": "Show author details",
        "hint": "Reveal scene IDs, source files, and diagnostics." }
    ] },
    { "id": "scene-launch", "label": "Launch defaults", "page": "scenes", "settings": [
      { "key": "launch.strip", "type": "enum", "default": "-1",
        "options": ["-1", "1", "0"], "optionLabels": ["Use scene", "Always", "Never"],
        "label": "Strip actors", "hint": "Default apparel override for browser launches." },
      { "key": "launch.lock", "type": "enum", "default": "-1",
        "options": ["-1", "1", "0"], "optionLabels": ["Use scene", "Always", "Never"],
        "label": "Lock player controls", "hint": "Default player-control override for browser launches." },
      { "key": "launch.camera", "type": "enum", "default": "",
        "options": ["", "thirdperson_hold", "scene_orbit", "freefly", "vanity_orbit"],
        "optionLabels": ["Use scene", "Third person", "Scene orbit", "Free fly", "Vanity orbit"],
        "label": "Camera", "hint": "Default camera policy for browser launches." },
      { "key": "launch.speed", "type": "enum", "default": "1",
        "options": ["0.5", "0.75", "1", "1.25", "1.5", "2"],
        "optionLabels": ["0.5x", "0.75x", "1x", "1.25x", "1.5x", "2x"],
        "label": "Playback speed", "hint": "Default clock multiplier for browser launches." }
    ] },
    { "id": "scene-gear", "label": "Scene gear", "page": "scenes", "settings": [
      { "key": "gear.autoEquip", "type": "bool", "default": true,
        "label": "Auto-equip scene gear",
        "hint": "Equip registered gear (belts, props, etc...) carried by scene participants for the scene's duration. Register items via *.osfgear.json files." }
    ] },
    { "id": "diagnostics", "label": "Diagnostics", "page": "advanced", "settings": [
      { "key": "debugNotifications", "type": "bool", "default": false,
        "label": "Stage-transition popups",
        "hint": "Debug HUD popup on each scene stage transition." },
      { "key": "logLevel", "type": "enum", "default": "info",
        "options": ["trace", "debug", "info", "warn", "error"],
        "optionLabels": ["Trace", "Debug", "Info", "Warnings", "Errors"],
        "label": "Log level", "hint": "OSF Animation.log verbosity." }
    ] }
  ]
})json";

		// The version-gated wrapper (header 1.6), initialized once at install
		// (the export returns OSF UI's singleton — the same object UIBridge
		// wraps). Unconnected => OSF UI absent; calls degrade to no-ops.
		OSFUI::API::Client g_bridge;

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
			} else if (key == "gear.autoEquip") {
				Equipment::Gear::SetAutoEquip(value == "true");
			}
		}

		// SubscribeHotkey sinks — the press already passed OSF UI's gates
		// (console closed, no capture, no rebind armed). Game main thread.
		void OnHotkey(const char* /*a_modId*/, const char* a_key, void*) noexcept
		{
			const std::string_view key{ a_key ? a_key : "" };
			if (key == "hotkeys.openBrowser") {
				OpenBrowser();
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

		if (!g_bridge.Init()) {
			REX::INFO("[Config] OSF UI not present — settings menu + hotkeys unavailable, defaults in effect");
			return;
		}

		if (!g_bridge.Has(Feature::kSettings)) {
			REX::WARN("[Config] installed OSF UI has no settings surface (bridge MINOR < 2) — "
			          "update OSF UI for the settings menu; defaults in effect");
			g_bridge.Attach(nullptr);
			return;
		}

		if (!g_bridge.RegisterSettingsSchema(kSchemaJson)) {
			REX::ERROR("[Config] OSF UI rejected the settings schema — defaults in effect");
			g_bridge.Attach(nullptr);
			return;
		}
		g_bridge.SubscribeSettings("osf.animation", &OnSetting, nullptr);

		if (g_bridge.Has(Feature::kHotkeys)) {
			g_bridge.SubscribeHotkey("osf.animation", "hotkeys.openBrowser", &OnHotkey, nullptr);
			REX::INFO("[Feature] MCM settings CONNECTED (schema 'osf.animation' registered, browser hotkey subscribed)");
		} else {
			REX::WARN("[Feature] MCM settings CONNECTED, but hotkey dispatch needs OSF UI bridge MINOR >= 4 — hotkeys inert");
		}
	}
}
