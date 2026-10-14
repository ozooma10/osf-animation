#pragma once
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

namespace OSF::API::EngineSettingsMigration
{
	// The released Settings 1.0 store is sparse formatVersion=1 JSON. Read only our legacy values;
	// writes go through Settings so its live state and atomic persistence stay authoritative.
	inline bool Run(const std::filesystem::path& directory, const std::function<bool(const std::string&, const nlohmann::json&)>& write)
	{
		using json = nlohmann::json;
		const auto receipt = directory / "osf.animation.engine.migrated";
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
			const auto target = read("osf.animation.engine.json");
			for (const char* key : { "logLevel", "debugNotifications", "gear.autoEquip" }) {
				if (!source.contains(key) || target.contains(key))
					continue;
				const auto& value = source.at(key);
				const bool  valid = std::string_view(key) == "logLevel" ? value.is_string() && (value == "trace" || value == "debug" || value == "info" || value == "warn" || value == "error") : value.is_boolean();
				if (valid && !write(key, value))
					return false;
			}
			std::filesystem::create_directories(directory);
			const auto    temporary = directory / "osf.animation.engine.migrated.tmp";
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
