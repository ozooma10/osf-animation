#include "Config/Settings.h"

#include "Audio/SoundService.h"
#include "UI/HudMessage.h"

#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace OSF::Config::Settings
{
	namespace
	{
		// "trace" | "debug" | "info" | "warn" | "error" -> spdlog level, applied to the live logger.
		// The default (no settings.json) is build-driven: Debug in debug builds, Info in the shipped  releasedbg/release build (SFSE::InitInfo::logLevel).
		void SetLogLevel(std::string_view a_level)
		{
			std::string s;
			s.reserve(a_level.size());
			for (const char c : a_level) {
				s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}

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
				REX::ERROR("[Config] logLevel '{}' unrecognized (use trace|debug|info|warn|error) — keeping current", a_level);
				return;
			}

			if (const auto logger = spdlog::default_logger()) {
				logger->set_level(lvl);
				logger->flush_on(lvl);
			}
			REX::INFO("[Config] log level set to '{}'", s);
		}
	}

	void Load()
	{
		const auto file = std::filesystem::current_path() / "Data" / "OSF" / "settings.json";

		std::error_code ec;
		if (!std::filesystem::is_regular_file(file, ec)) {
			REX::INFO("[Config] no Data/OSF/settings.json — using defaults (see Settings.h for the schema)");
			return;
		}

		nlohmann::json json;
		try {
			std::ifstream in(file, std::ios::binary);
			json = nlohmann::json::parse(in, nullptr, true, true);  // tolerate // comments
			if (!json.is_object()) {
				REX::ERROR("[Config] settings.json is not a JSON object — using defaults");
				return;
			}
		} catch (const std::exception& e) {
			REX::ERROR("[Config] failed to parse settings.json ({}) — using defaults", e.what());
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
					REX::ERROR("[Config] '{}' must be a boolean — ignored", a_key);
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
					REX::ERROR("[Config] '{}' must be a number — ignored", a_key);
				}
				json.erase(it);
			}
		};
		auto stringKey = [&](const char* a_key, auto&& a_apply) {
			if (auto it = json.find(a_key); it != json.end()) {
				if (it->is_string()) {
					a_apply(it->get<std::string>());
					applied++;
				} else {
					REX::ERROR("[Config] '{}' must be a string — ignored", a_key);
				}
				json.erase(it);
			}
		};

		// logLevel first, so the rest of startup honours the chosen verbosity.
		stringKey("logLevel", [](const std::string& v) { SetLogLevel(v); });

		floatKey("soundVolume", [](float v) { Audio::SoundService::GetSingleton().SetVolume(v); });

		boolKey("debugNotifications", [](bool v) { UI::HudMessage::SetDebugEnabled(v); });

		for (const auto& [key, value] : json.items()) {
			REX::WARN("[Config] unrecognized key '{}' — ignored (typo, or a newer OSF Animation?)", key);
		}

		REX::INFO("[Config] applied {} setting(s) from Data/OSF/settings.json", applied);
	}
}
