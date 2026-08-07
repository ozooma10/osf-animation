#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace OSF::Props
{
	enum class SourceKind : std::uint8_t
	{
		kForm,
		kEquippedArmor
	};

	// A scene prop's visual source. A fixed form uses the normal Plugin|LocalID
	// syntax. An equipped-armor source selects the first worn ARMO carrying any
	// of the authored keyword editor IDs.
	struct Source
	{
		SourceKind               kind = SourceKind::kForm;
		std::string              form;
		std::vector<std::string> keywords;
	};

	struct Attachment
	{
		std::string          targetNode;
		std::array<float, 3> position{};
		std::array<float, 3> rotation{};
		float                scale = 1.0f;
	};
}
