#include "Serialization/BrowserPreferences.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

namespace OSF::Serialization
{
	namespace
	{
		using json = nlohmann::json;
		// Retired keys (browser.openTo, browser.unavailableScenes) are absent here, so Valid
		// rejects them and an older preference file loses them on the next save.
		const json kDefaults = {
			{ "browser.afterLaunch", "stay" },
			{ "browser.actorLabels", true }, { "browser.authorDetails", false },
			{ "browser.libraryDetail", "curated" }, { "browser.librarySource", "all" },
			{ "launch.strip", "-1" }, { "launch.lock", "-1" }, { "launch.camera", "scene" }
		};
		const json kChoices = {
			{ "browser.afterLaunch", { "minimize", "stay", "close" } },
			{ "browser.libraryDetail", { "curated", "full" } },
			{ "browser.librarySource", { "all", "custom" } },
			{ "launch.strip", { "-1", "1", "0" } }, { "launch.lock", { "-1", "1", "0" } },
			{ "launch.camera", { "scene", "thirdperson_hold", "scene_orbit", "freefly", "vanity_orbit" } }
		};

		json Read(const std::filesystem::path& file)
		{
			if (!std::filesystem::exists(file)) return json::object();
			if (std::filesystem::file_size(file) > 1024 * 1024)
				throw std::runtime_error("preference file is too large");
			std::ifstream input(file, std::ios::binary);
			const auto document = json::parse(input);
			if (!document.is_object() || !document.contains("formatVersion") ||
				!document.at("formatVersion").is_number_integer() || document.at("formatVersion") != 1 ||
				!document.contains("values") || !document.at("values").is_object())
				throw std::runtime_error("unsupported preference file");
			return document.at("values");
		}

		void Apply(json& values, const json& saved)
		{
			for (const auto& [key, value] : saved.items()) {
				if (BrowserPreferences::Valid(key, value)) values[key] = value;
			}
		}
	}

	BrowserPreferences::BrowserPreferences(std::filesystem::path file) :
		_file(std::move(file)), _values(kDefaults)
	{}

	bool BrowserPreferences::Valid(std::string_view key, const json& value)
	{
		const auto defaultValue = kDefaults.find(key);
		if (defaultValue == kDefaults.end()) return false;
		if (defaultValue->is_boolean()) return value.is_boolean();
		const auto choices = kChoices.find(key);
		return value.is_string() && choices != kChoices.end() &&
			std::find(choices->begin(), choices->end(), value) != choices->end();
	}

	bool BrowserPreferences::Load(std::string& error)
	{
		error.clear();
		if (_loaded) return true;
		try {
			auto values = kDefaults;
			if (!_file.empty()) {
				Apply(values, Read(_file));
			}
			_values = std::move(values);
			_loaded = true;
			return true;
		} catch (const std::exception& e) {
			error = e.what();
			return false;
		}
	}

	bool BrowserPreferences::Set(std::string_view key, const json& value, std::string& error)
	{
		error.clear();
		if (!Valid(key, value)) {
			error = "unknown preference or invalid value";
			return false;
		}
		if (!Load(error)) return false;
		if (_values.at(std::string(key)) == value) return true;
		auto next = _values;
		next[std::string(key)] = value;
		if (!Save(next, error)) return false;
		_values = std::move(next);
		return true;
	}

	bool BrowserPreferences::Save(const json& values, std::string& error) const
	{
		if (_file.empty()) return true;
		try {
			std::filesystem::create_directories(_file.parent_path());
			auto temporary = _file;
			temporary += ".tmp";
			std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
			output << json{ { "formatVersion", 1 }, { "values", values } }.dump(2) << '\n';
			output.close();
			if (!output) {
				error = "could not write browser preferences";
				return false;
			}
			if (!::MoveFileExW(temporary.c_str(), _file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				error = "could not replace browser preferences (Win32 error " + std::to_string(::GetLastError()) + ")";
				return false;
			}
			return true;
		} catch (const std::exception& e) {
			error = e.what();
			return false;
		}
	}
}
