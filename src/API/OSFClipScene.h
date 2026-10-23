#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace OSF::API
{
	// Serialized provider output, never passed as a C++ object across DLLs. A clip scene
	// references one inline clip from the provider's immutable input catalog.
	struct ClipScene
	{
		std::string id, source, name;
		std::uint32_t stage{}, clip{};
		std::vector<std::string> tags;
		bool unlisted{}, library{}, followActor{}, playerInputLock{true}, hideApparel{true}, fade{}, clearOffset{};
		NLOHMANN_DEFINE_TYPE_INTRUSIVE(ClipScene, id, source, name, stage, clip, tags,
			unlisted, library, followActor, playerInputLock, hideApparel, fade, clearOffset)
	};
}
