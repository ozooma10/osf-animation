#pragma once

// Process-local clip durations, keyed on normalized clip spec + anim id.
// Catalog enrichment and persistence live in ClipDurationCatalog.

#include <optional>
#include <string_view>

namespace OSF::Serialization::ClipDurations
{
	// Memory only; unknown values remain empty until playback records them or a catalog scan finishes.
	// a_fileSpec is the authored spec as written in scene JSON.
	std::optional<float> Lookup(std::string_view a_fileSpec, std::string_view a_animId);

	// Memory only: never reads/writes the persistent cache or schedules background work.
	void Record(std::string_view a_fileSpec, std::string_view a_animId, float a_seconds);
}
