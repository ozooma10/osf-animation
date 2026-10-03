#pragma once

// Persistent clip-length cache (SFSE\OSF\clip-durations.json) for the catalog's time estimates.
// Keyed on normalized clip spec + anim id; loose files are invalidated by size+mtime. Decoded values (Record) override probes.

#include <functional>
#include <optional>
#include <string_view>

namespace OSF::Serialization::ClipDurations
{
	// a_fileSpec is the authored spec as written in scene JSON.
	std::optional<float> Lookup(std::string_view a_fileSpec, std::string_view a_animId);

	// A value confirming an existing probe isn't written immediately, so preloading a scene doesn't rewrite the file per clip.
	void Record(std::string_view a_fileSpec, std::string_view a_animId, float a_seconds);

	// Background thread, engine-free IO only. Probes, prunes unreferenced entries, and persists.
	void ScanSceneClipsAsync(std::function<void()> a_onChanged);
}
