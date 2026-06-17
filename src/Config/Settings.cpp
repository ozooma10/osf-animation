#include "Config/Settings.h"

#include "Audio/SoundService.h"
#include "Equipment/EquipmentService.h"
#include "UI/FadeService.h"
#include "Weapon/WeaponService.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace OSF::Config::Settings
{
	void Load()
	{
		const auto file = std::filesystem::current_path() / "Data" / "OSF" / "settings.json";

		std::error_code ec;
		if (!std::filesystem::is_regular_file(file, ec)) {
			REX::INFO("Settings: no Data/OSF/settings.json — using defaults (see Settings.h for the schema)");
			return;
		}

		nlohmann::json json;
		try {
			std::ifstream in(file, std::ios::binary);
			json = nlohmann::json::parse(in, nullptr, true, true);  // tolerate // comments
			if (!json.is_object()) {
				REX::ERROR("Settings: settings.json is not a JSON object — using defaults");
				return;
			}
		} catch (const std::exception& e) {
			REX::ERROR("Settings: failed to parse settings.json ({}) — using defaults", e.what());
			return;
		}

		// Apply through the services' own setters so clamps + effective-state semantics match.
		std::size_t applied = 0;
		auto boolKey = [&](const char* a_key, auto&& a_apply) {
			if (auto it = json.find(a_key); it != json.end()) {
				if (it->is_boolean()) {
					a_apply(it->get<bool>());
					applied++;
				} else {
					REX::ERROR("Settings: '{}' must be a boolean — ignored", a_key);
				}
				json.erase(it);
			}
		};
		auto floatKey = [&](const char* a_key, auto&& a_apply) {
			if (auto it = json.find(a_key); it != json.end()) {
				if (it->is_number()) {
					a_apply(it->get<float>());
					applied++;
				} else {
					REX::ERROR("Settings: '{}' must be a number — ignored", a_key);
				}
				json.erase(it);
			}
		};

		boolKey("soundEnabled", [](bool v) { Audio::SoundService::GetSingleton().SetEnabled(v); });
		floatKey("soundVolume", [](float v) { Audio::SoundService::GetSingleton().SetVolume(v); });
		boolKey("fadeEnabled", [](bool v) { UI::FadeService::GetSingleton().SetEnabled(v); });
		boolKey("equipmentEnabled", [](bool v) { Equipment::EquipmentService::GetSingleton().SetEnabled(v); });
		// Deprecated alias for the content-neutral rename (equipment == the old "undress").
		boolKey("undressEnabled", [](bool v) { Equipment::EquipmentService::GetSingleton().SetEnabled(v); });
		boolKey("weaponEnabled", [](bool v) { Weapon::WeaponService::GetSingleton().SetEnabled(v); });
		// Milestone-0 diagnostic: when true, post the PCM test clip as a Wwise external source at
		// startup and log the playingID. Off by default; harmless to leave on (one beep at the menu).
		boolKey("wwiseSelfTest", [](bool v) { if (v) Audio::SoundService::GetSingleton().RunWwiseSelfTest(); });

		// Anything still here is either a typo or a key from a newer build. Warn loudly
		// enough that someone can figure out why their setting didn't take effect.
		for (const auto& [key, value] : json.items()) {
			REX::WARN("Settings: unrecognized key '{}' — ignored (typo, or a newer OSF Animation?)", key);
		}

		REX::INFO("Settings: applied {} setting(s) from Data/OSF/settings.json", applied);
	}
}
