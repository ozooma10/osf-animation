#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Minimal reader for Starfield BA2 "GNRL" (general) archives 

namespace OSF::Util::Ba2
{
	// If `a_preferredArchive` is given (like "Starfield - Animations.ba2"), tried first as a fast path. Falls back to a full scan if it isn't there
	std::optional<std::vector<std::uint8_t>> ReadGameFile(std::string_view a_relPath,
		std::string_view a_preferredArchive = {});
}
