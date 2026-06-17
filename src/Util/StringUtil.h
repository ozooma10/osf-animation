#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace OSF::Util
{
	// Locale-independent ASCII lowercase copy. Used for our case-insensitive
	// keys: animation ids, voice-set names, file extensions.
	inline std::string ToLower(std::string_view a_str)
	{
		std::string result(a_str);
		for (auto& c : result) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return result;
	}
}
