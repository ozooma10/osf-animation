#include "API/EngineSettings.h"
#include "API/EngineSettingsMigration.h"
#include "API/OSFSettings.h"
#include "Equipment/GearRegistry.h"
#include "UI/HudMessage.h"

#include <spdlog/spdlog.h>
#include <mutex>
#include <string_view>

namespace OSF::API
{
	namespace
	{
		using OSFSettings::API::Status;
		constexpr const char* kModId = "osf.animation.engine";
		OSFSettings::API::Client g_settings;
		OSFSettings::API::Subscription g_subscription{};
		std::mutex g_refreshMutex;
		bool g_migrated{};

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
#ifndef OSF_ANIMATION_SMOKE
			// Private harness hosts may redirect Settings storage; never migrate player files in that build.
			if (!g_migrated && g_settings.IsReady()) {
				if (const auto logs = SFSE::log::log_directory()) {
					g_migrated = EngineSettingsMigration::Run(logs->parent_path().parent_path() / "OSF/Settings",
						[](const std::string& key, const nlohmann::json& value) {
							return (value.is_boolean() ? g_settings.SetBool(kModId, key.c_str(), value.get<bool>()) :
								g_settings.SetEnum(kModId, key.c_str(), value.get_ref<const std::string&>().c_str())) == Status::Ok;
						});
					if (!g_migrated) REX::WARN("[Config] Engine settings migration incomplete; will retry without replacing saved engine values");
				}
			}
#endif
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

		}

		void OnSetting(const char*, const char* a_key, void*) noexcept
		{
			try { RefreshSettings(a_key ? a_key : ""); }
			catch (...) { REX::ERROR("[Config] Could not refresh OSF Settings values"); }
		}

	}

	void InstallEngineSettings()
	{
		if (!g_settings.Init()) {
			REX::WARN("[Config] OSF Settings unavailable; engine settings use defaults");
			return;
		}
		const auto status = g_settings.Subscribe(kModId, &OnSetting, nullptr, &g_subscription);
		if (status != Status::Ok) {
			REX::ERROR("[Config] OSF Settings subscription failed (status {}); check osf.animation.engine.json", static_cast<unsigned>(status));
			return;
		}
		OnSetting(nullptr, nullptr, nullptr);
		REX::INFO("[Feature] OSF Settings connected");
	}
}
