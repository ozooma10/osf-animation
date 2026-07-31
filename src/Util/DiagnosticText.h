#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace OSF::Util
{
	// Health context can be copied into a consented diagnostic report. Replace quoted
	// absolute paths with their final component while leaving ordinary author text intact.
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
				// A malformed token is safer left unchanged than allowed to disrupt reporting.
			}
			cursor = end + 1;
		}
		return result;
	}
}
