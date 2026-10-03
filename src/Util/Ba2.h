#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Reads GNRL (general) BA2 archives only.
namespace OSF::Util::Ba2
{
	// a_preferredArchive is tried first; a full Data scan covers archive renames/splits.
	std::optional<std::vector<std::uint8_t>> ReadGameFile(std::string_view a_relPath,
		std::string_view a_preferredArchive = {});
}
