#pragma once

// Persistent clip-length cache feeding the UI catalog's time estimates.
// a GLB's keyframe times are declared in its JSON chunk (accessor max / last input key), a .af's frame count sits in its 64-byte header
//
// Values live in <Documents>\My Games\Starfield\OSF\clip-durations.json, keyed on the authored clip spec (normalized) + animation id, invalidated by loose-file size+mtime. 
// Real decodes overwrite probe values (Record), so the two sources converge on the exact number.

#include <functional>
#include <optional>
#include <string_view>

namespace OSF::Serialization::ClipDurations
{
	// Clip loop length in seconds, or nullopt when the clip hasn't been probed or decoded yet (missing file, unprobeable metadata, scan still running). 
	// a_fileSpec is the authored spec string exactly as it appears in scene JSON
	std::optional<float> Lookup(std::string_view a_fileSpec, std::string_view a_animId);

	// Record the exact duration of a successfully decoded clip. Writes the cache file only for genuinely new information
	// a probe-confirming value just flips the exact flag in memory, so a scene preloading all its stages never pays one file rewrite per clip.
	void Record(std::string_view a_fileSpec, std::string_view a_animId, float a_seconds);

	// Probe every clip referenced by the loaded scene registry on a background thread (engine-free file IO only), refresh stale cache entries, prune entries nothing references any more, and persist. 
	void ScanSceneClipsAsync(std::function<void()> a_onChanged);
}
