#include "API/BrowserSession.h"
#include "API/UISettings.h"
#include "Serialization/BrowserPreferences.h"
#include "OSFSettings.h"
#include "OSFUI.h"
#include "API/UIBridge.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <memory>
#include <mutex>
#include <string_view>

namespace OSF::API
{
	namespace
	{
		using json = nlohmann::json;
		using OSFSettings::API::Status;
		constexpr const char* kModId = "osf.animation.browser";
		constexpr const char* kStateNamespace = "osf.animation";
		constexpr const char* kEngineModId = "osf.animation.engine";
		constexpr const char* kViewId = "osf.animation/browser";
		OSFSettings::API::Client g_settings;
		OSFUI::API::Client g_views;
		OSFSettings::API::Subscription g_engineSubscription{};
		std::mutex g_refreshMutex;
		std::unique_ptr<Serialization::BrowserPreferences> g_preferences;

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

		void PublishPreferences()
		{
			// Caller holds g_refreshMutex. Only validated Browser values cross the web boundary.
			if (!g_views || !g_preferences) return;
			const auto payload = g_preferences->Values().dump();
			if (!g_views.SetState(kStateNamespace, "settings", payload.c_str())) {
				REX::WARN("[Config] Could not publish browser preferences");
			}
		}

		void OnSetting(const char*, const char* a_key, void*) noexcept
		{
			try {
				std::lock_guard lock(g_refreshMutex);
				if (!a_key || !*a_key || std::string_view(a_key) == "logLevel") {
					std::string level;
					if (g_settings.GetEnum(kEngineModId, "logLevel", level) == Status::Ok) SetLogLevel(level);
				}
			}
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
				if (!Serialization::BrowserPreferences::Valid(key, value)) {
					request.Reject("invalid-argument", "Unknown browser preference or invalid value");
					return;
				}
				{
					std::lock_guard lock(g_refreshMutex);
					std::string error;
					if (!g_preferences || !g_preferences->Set(key, value, error)) {
						REX::ERROR("[Config] Could not save Browser preferences: {}", error.empty() ? "storage unavailable" : error);
						request.Reject("setting-write-failed", "Could not save browser preferences");
						return;
					}
					PublishPreferences();
				}
				request.Reply("{}");
			} catch (...) { request.Reject("internal-error", "Could not save the browser preference"); }
		}
	}

	void InstallUISettings()
	{
		if (!BrowserSession::Available()) return;
		g_views.Init();
		{
			std::lock_guard lock(g_refreshMutex);
#ifdef OSF_ANIMATION_SMOKE
			// The private harness must never read or write player preferences.
			g_preferences = std::make_unique<Serialization::BrowserPreferences>(std::filesystem::path{});
#else
			if (const auto logs = SFSE::log::log_directory()) {
				g_preferences = std::make_unique<Serialization::BrowserPreferences>(
					logs->parent_path() / "OSF/browser-preferences.json");
			}
#endif
			std::string error;
			if (!g_preferences || !g_preferences->Load(error)) {
				REX::ERROR("[Config] Could not load Browser preferences: {}", error.empty() ? "storage unavailable" : error);
			}
			PublishPreferences();
		}
		if (g_views && !g_views.RegisterRequest("osf.animation.settings.set", &OnSetPreference, nullptr)) {
			REX::ERROR("[Config] Could not register the browser preference endpoint");
		}
		if (!g_settings.Init()) {
			REX::WARN("[Config] OSF Settings unavailable; Browser hotkeys and shared log level are unavailable");
			return;
		}
		if (g_settings.Subscribe(kEngineModId, &OnSetting, nullptr, &g_engineSubscription) != Status::Ok) {
			REX::WARN("[Config] Could not subscribe to engine log-level changes");
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
