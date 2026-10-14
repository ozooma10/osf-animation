#pragma once

// Process-local clip durations, keyed on normalized clip spec + anim id.
// Browser enrichment and persistence are owned by the optional addon.

#include <optional>
#include <string_view>
#include "API/OSFCatalog.h"

namespace OSF::Serialization::ClipDurations
{
	// Memory only; unknown values remain empty until playback records them.
	// a_fileSpec is the authored spec as written in scene JSON.
	std::optional<float> Lookup(std::string_view a_fileSpec, std::string_view a_animId);

	// Memory only: never reads/writes the persistent cache or schedules background work.
	void Record(std::string_view a_fileSpec, std::string_view a_animId, float a_seconds);
	std::string KeyFor(std::string_view a_fileSpec, std::string_view a_animId);
	Catalog::DecodedDurations Snapshot();
}
