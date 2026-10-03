#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace OSF::Util
{
	// Quoted absolute paths -> filename only; health context can end up in shared reports.
	inline std::string SanitizeDiagnosticPaths(std::string_view a_text)
	{
		std::string result{ a_text };
		std::size_t cursor = 0;
		while (cursor < result.size()) {
			const auto begin = result.find_first_of("'\"", cursor);
			if (begin == std::string::npos) {
				break;
			}
			const char quote = result[begin];
			const auto end = result.find(quote, begin + 1);
			if (end == std::string::npos) {
				break;
			}

			const std::string token = result.substr(begin + 1, end - begin - 1);
			try {
				const std::filesystem::path path{ token };
				if (path.is_absolute()) {
					std::string label = path.filename().string();
					if (label.empty()) {
						label = "<path>";
					}
					result.replace(begin + 1, end - begin - 1, label);
					cursor = begin + label.size() + 2;
					continue;
				}
			} catch (...) {
				// Leave malformed tokens unchanged.
			}
			cursor = end + 1;
		}
		return result;
	}
}
