#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace OSF::Util
{
	inline std::string AnimationKeywordLabel(std::string_view a_editorId)
	{
		for (const std::string_view prefix : { "AnimFurn", "Anim" }) {
			if (a_editorId.starts_with(prefix)) {
				a_editorId.remove_prefix(prefix.size());
				break;
			}
		}

		std::string label;
		label.reserve(a_editorId.size() + 8);
		for (std::size_t i = 0; i < a_editorId.size(); ++i) {
			const char c = a_editorId[i];
			// Break lower/digit-to-upper and acronym-to-word boundaries.
			if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
				(!std::isupper(static_cast<unsigned char>(a_editorId[i - 1])) ||
					(i + 1 < a_editorId.size() && std::islower(static_cast<unsigned char>(a_editorId[i + 1]))))) {
				label += ' ';
			}
			label += (c == '_') ? ' ' : c;
		}
		return label;
	}
}
