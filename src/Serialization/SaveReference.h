#pragma once

#include <array>
#include <cstdint>

namespace OSF::Serialization
{
	// The save writer encodes generated references in 22 bits plus the 0x80 tag.
	// Reject IDs outside that range instead of aliasing an unrelated reference.
	inline bool EncodeGeneratedSaveReference(std::uint32_t formID, std::array<std::uint8_t, 3>& bytes)
	{
		bytes = {};
		if ((formID >> 24) != 0xFF || (formID & 0xFFFFFF) > 0x3FFFFF) {
			return false;
		}
		bytes = { static_cast<std::uint8_t>(0x80 | ((formID >> 16) & 0x3F)),
			static_cast<std::uint8_t>(formID >> 8), static_cast<std::uint8_t>(formID) };
		return true;
	}
}
