#include "API/UISettings.h"
#include "API/OSFSettings.h"
#include "API/OSFUI.h"
#include "API/UIBridge.h"
#include "Equipment/GearRegistry.h"
#include "Studio/StudioPreviewService.h"
#include "UI/HudMessage.h"
#include "Util/NativeMainThreadQueue.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <mutex>
#include <string_view>

namespace OSF::API
{
	namespace
	{
		using json = nlohmann::json;
		using OSFSettings::API::Status;
		constexpr const char* kModId = "osf.animation";
		constexpr const char* kViewId = "osf.animation/browser";
		OSFSettings::API::Client g_settings;
		OSFUI::API::Client g_views;
		OSFSettings::API::Subscription g_subscription{};
		std::mutex g_refreshMutex;

		// Only browser preferences cross the web boundary; native settings and other mods stay private.
		constexpr const char* kBoolPreferences[] = {
			"browser.rememberBrowsing", "browser.actorLabels", "browser.authorDetails"
		};
		constexpr const char* kEnumPreferences[] = {
			"browser.afterLaunch", "browser.openTo", "browser.libraryDetail", "browser.librarySource",
			"browser.unavailableScenes", "launch.strip", "launch.lock", "launch.camera", "launch.speed"
		};

		void SetLogLevel(std::string_view a_level)
		{
			spdlog::level::level_enum lvl = spdlog::level::info;
			if (a_level == "trace") {
				lvl = spdlog::level::trace;
			} else if (a_level == "debug") {
				lvl = spdlog::level::debug;
			} else if (a_level == "info") {
				lvl = spdlog::level::info;
			} else if (a_level == "warn") {
				lvl = spdlog::level::warn;
			} else if (a_level == "error") {
				lvl = spdlog::level::err;
			} else {
				REX::ERROR("[Config] logLevel '{}' unrecognized — keeping current", a_level);
				return;
			}
			if (const auto logger = spdlog::default_logger()) {
				logger->set_level(lvl);
				logger->flush_on(lvl);
			}
			REX::DEBUG("[Config] log level set to '{}'", a_level);
		}

		void RefreshSettings(std::string_view a_key)
		{
			// Initial reads and SFSE worker callbacks may overlap. Read after subscribing.
			std::lock_guard lock(g_refreshMutex);
			const bool refreshAll = a_key.empty();
			if (refreshAll || a_key == "logLevel") {
				std::string level;
				if (g_settings.GetEnum(kModId, "logLevel", level) == Status::Ok) SetLogLevel(level);
			}
			bool enabled{};
			if ((refreshAll || a_key == "debugNotifications") &&
				g_settings.GetBool(kModId, "debugNotifications", &enabled) == Status::Ok) {
				UI::HudMessage::SetDebugEnabled(enabled);
			}
			if ((refreshAll || a_key == "gear.autoEquip") &&
				g_settings.GetBool(kModId, "gear.autoEquip", &enabled) == Status::Ok) {
				Equipment::Gear::SetAutoEquip(enabled);
			}
			if ((refreshAll || a_key == "studioLink.enabled") &&
				g_settings.GetBool(kModId, "studioLink.enabled", &enabled) == Status::Ok) {
				Util::NativeMainThreadQueue::Post([enabled]() { Studio::SetPreviewServiceEnabled(enabled); });
			}

			const bool browserChanged = refreshAll || std::ranges::contains(kBoolPreferences, a_key) ||
				std::ranges::contains(kEnumPreferences, a_key);
			if (!g_views || !browserChanged) return;
			json values = json::object();
			for (const auto* key : kBoolPreferences) {
				bool value{};
				if (g_settings.GetBool(kModId, key, &value) == Status::Ok) values[key] = value;
			}
			for (const auto* key : kEnumPreferences) {
				std::string value;
				if (g_settings.GetEnum(kModId, key, value) == Status::Ok) values[key] = value;
			}
			const auto payload = values.dump();
			if (!g_views.SetState(kModId, "settings", payload.c_str())) {
				REX::WARN("[Config] Could not publish browser preferences");
			}
		}

		void OnSetting(const char*, const char* a_key, void*) noexcept
		{
			try { RefreshSettings(a_key ? a_key : ""); }
			catch (...) { REX::ERROR("[Config] Could not refresh OSF Settings values"); }
		}

		void OnHotkey(const char*, const char* a_id, void*) noexcept
		{
			try {
				const std::string_view id = a_id ? a_id : "";
				if (id == "openBrowser") OpenBrowser();
				else if (id == "openWheel") OpenWheel("");
			} catch (...) { REX::ERROR("[Hotkey] Could not open animation view"); }
		}

		void OnSetPreference(const OSFUI::API::Request& request, void*) noexcept
		{
			try {
				if (!request.sourceViewId || std::string_view(request.sourceViewId) != kViewId) {
					request.Reject("forbidden", "Only the animation browser can write these preferences");
					return;
				}
				const auto payload = json::parse(request.payloadJson ? request.payloadJson : "", nullptr, false);
				if (!payload.is_object() || !payload.contains("key") || !payload["key"].is_string() || !payload.contains("value")) {
					request.Reject("invalid-argument", "Expected an animation preference key and value");
					return;
				}
				const auto key = payload["key"].get<std::string>();
				const auto& value = payload["value"];
				auto status = Status::UnknownSetting;
				for (const auto* allowed : kBoolPreferences) {
					if (key == allowed) status = value.is_boolean() ? g_settings.SetBool(kModId, allowed, value.get<bool>()) : Status::TypeMismatch;
				}
				for (const auto* allowed : kEnumPreferences) {
					if (key == allowed) status = value.is_string() ? g_settings.SetEnum(kModId, allowed, value.get_ref<const std::string&>().c_str()) : Status::TypeMismatch;
				}
				if (status != Status::Ok) {
					request.Reject("setting-write-failed", "OSF Settings could not save this preference");
					return;
				}
				request.Reply("{}");
			} catch (...) { request.Reject("internal-error", "Could not save the browser preference"); }
		}
	}

	void InstallUISettings()
	{
		Studio::SetPreviewServiceEnabled(false);
		g_views.Init();
		if (!g_settings.Init()) {
			REX::WARN("[Config] OSF Settings unavailable; settings and hotkeys use defaults");
			return;
		}
		if (g_views && !g_views.RegisterRequest("osf.animation.settings.set", &OnSetPreference, nullptr)) {
			REX::ERROR("[Config] Could not register the browser preference endpoint");
		}
		const auto status = g_settings.Subscribe(kModId, &OnSetting, nullptr, &g_subscription);
		if (status != Status::Ok) {
			REX::ERROR("[Config] OSF Settings subscription failed (status {}); check osf.animation.json", static_cast<unsigned>(status));
			return;
		}
		OnSetting(nullptr, nullptr, nullptr);
		for (const char* id : { "openBrowser", "openWheel" }) {
			if (g_settings.RegisterHotkey(kModId, id, &OnHotkey, nullptr) != Status::Ok) {
				REX::ERROR("[Hotkey] Could not register '{}' with OSF Settings", id);
			}
		}
		REX::INFO("[Feature] OSF Settings connected");
	}
}
