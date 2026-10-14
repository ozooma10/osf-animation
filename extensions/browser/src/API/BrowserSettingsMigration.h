#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

namespace OSF::API::BrowserSettingsMigration
{
	// Only these preferences belonged to the browser in the legacy combined Settings schema.
	inline bool Valid(const std::string& key, const nlohmann::json& value)
	{
		if (key == "browser.rememberBrowsing" || key == "browser.actorLabels" || key == "browser.authorDetails")
			return value.is_boolean();
		static const nlohmann::json options = { { "browser.afterLaunch", { "minimize", "stay", "close" } }, { "browser.openTo", { "last", "scenes", "active" } }, { "browser.libraryDetail", { "curated", "full" } }, { "browser.librarySource", { "all", "custom" } },
			{ "browser.unavailableScenes", { "ask", "show", "hide" } }, { "launch.strip", { "-1", "1", "0" } }, { "launch.lock", { "-1", "1", "0" } }, { "launch.camera", { "scene", "thirdperson_hold", "scene_orbit", "freefly", "vanity_orbit" } },
			{ "launch.speed", { "0.5", "0.75", "1", "1.25", "1.5", "2" } } };
		const auto                  allowed = options.find(key);
		return value.is_string() && allowed != options.end() && std::find(allowed->begin(), allowed->end(), value) != allowed->end();
	}

	// Settings 1.0 stores sparse formatVersion=1 JSON. Writes go through its public API;
	// explicit destination values win, and a receipt prevents reimport after a later reset.
	inline bool Run(const std::filesystem::path& directory, const std::function<bool(const std::string&, const nlohmann::json&)>& write)
	{
		using json = nlohmann::json;
		const auto receipt = directory / "osf.animation.browser.migrated";
		try {
			if (std::filesystem::exists(receipt))
				return true;
			const auto read = [&](const char* name) {
				const auto path = directory / name;
				if (!std::filesystem::exists(path))
					return json::object();
				if (std::filesystem::file_size(path) > 1024 * 1024)
					throw std::runtime_error("oversized settings file");
				std::ifstream input(path);
				const auto    document = json::parse(input);
				if (document.value("formatVersion", 0) != 1 || !document.at("values").is_object())
					throw std::runtime_error("unsupported settings file");
				return document.at("values");
			};
			const auto source = read("osf.animation.json");
			const auto target = read("osf.animation.browser.json");
			for (const auto& [key, value] : source.items()) {
				if (!target.contains(key) && Valid(key, value) && !write(key, value))
					return false;
			}
			std::filesystem::create_directories(directory);
			const auto    temporary = directory / "osf.animation.browser.migrated.tmp";
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			output << "1\n";
			output.close();
			if (!output)
				return false;
			std::filesystem::rename(temporary, receipt);
			return true;
		} catch (...) {
			return false;
		}
	}
}
