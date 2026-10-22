#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace OSF::Serialization
{
	// Browser-owned preferences. The host serializes access; this core also runs in offline tests.
	class BrowserPreferences
	{
	public:
		// An empty path selects session-only storage for the private smoke harness.
		explicit BrowserPreferences(std::filesystem::path file);
		static bool Valid(std::string_view key, const nlohmann::json& value);
		bool Load(std::string& error);
		bool Set(std::string_view key, const nlohmann::json& value, std::string& error);
		const nlohmann::json& Values() const { return _values; }

	private:
		bool Save(const nlohmann::json& values, std::string& error) const;
		std::filesystem::path _file;
		nlohmann::json _values;
		bool _loaded{};
	};
}
