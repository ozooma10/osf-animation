#include "API/EngineSettings.h"
#include "API/OSFSettings.h"
#include "Equipment/GearRegistry.h"
#include "Input/SceneKeymap.h"

#include <spdlog/spdlog.h>
#include <mutex>
#include <string_view>

namespace OSF::API
{
	namespace
	{
		using OSFSettings::API::Status;
		constexpr const char* kModId = Input::kEngineModId;
		OSFSettings::API::Client g_settings;
		OSFSettings::API::Subscription g_subscription{};
		OSFSettings::API::Subscription g_alignSubscription{};
		std::mutex g_refreshMutex;

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

		// Alignment keys ship in their own schema, so a_mod selects which page's values to reread.
		// Both are empty for the initial full refresh.
		void RefreshSettings(std::string_view a_mod, std::string_view a_key)
		{
			// Initial reads and SFSE worker callbacks may overlap. Read after subscribing.
			std::lock_guard lock(g_refreshMutex);
			const bool everyMod = a_mod.empty();
			const bool everyKey = a_key.empty();
			if (everyMod || a_mod == kModId) {
				if (everyKey || a_key == "logLevel") {
					std::string level;
					if (g_settings.GetEnum(kModId, "logLevel", level) == Status::Ok) SetLogLevel(level);
				}
				bool enabled{};
				if ((everyKey || a_key == "gear.autoEquip") &&
					g_settings.GetBool(kModId, "gear.autoEquip", &enabled) == Status::Ok) {
					Equipment::Gear::SetAutoEquip(enabled);
				}
			}
			for (std::size_t i = 0; i < Input::kSceneKeys.size(); ++i) {
				const auto& key = Input::kSceneKeys[i];
				if (!Input::Configurable(key)) continue;  // fixed alias; no schema row to read
				if (!everyMod && !Input::OwnedBy(key, a_mod)) continue;
				if (!everyKey && a_key != key.setting) continue;
				std::uint32_t value = key.key;
				if (g_settings.GetKey(key.mod, key.setting, &value) == Status::Ok)
					Input::g_sceneKeymap.Set(i, value);
			}
		}

		void OnSetting(const char* a_mod, const char* a_key, void*) noexcept
		{
			try { RefreshSettings(a_mod ? a_mod : "", a_key ? a_key : ""); }
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
		// Alignment is a separate page. A hand-pruned install without it keeps the schema defaults.
		const auto alignStatus = g_settings.Subscribe(Input::kAlignModId, &OnSetting, nullptr, &g_alignSubscription);
		if (alignStatus != Status::Ok) {
			REX::WARN("[Config] alignment keys use defaults (status {}); check osf.animation.align.json",
				static_cast<unsigned>(alignStatus));
		}
		OnSetting(nullptr, nullptr, nullptr);
		REX::INFO("[Feature] OSF Settings connected");
	}
}
