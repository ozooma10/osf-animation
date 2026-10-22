#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace OSF::Util
{
	inline std::string ToLower(std::string_view a_str)
	{
		std::string result(a_str);
		for (auto& c : result) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return result;
	}

	// ASCII case-insensitive equality without allocating (ids, role names, plugin filenames).
	inline bool IEquals(std::string_view a_lhs, std::string_view a_rhs)
	{
		if (a_lhs.size() != a_rhs.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a_lhs.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(a_lhs[i])) != std::tolower(static_cast<unsigned char>(a_rhs[i]))) {
				return false;
			}
		}
		return true;
	}
}
