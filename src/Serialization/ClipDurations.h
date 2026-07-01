#pragma once

// Persistent clip-length cache feeding the UI catalog's time estimates. Durations normally only
// materialize when a clip is decoded for playback (GraphManager::LoadClip); this module makes them
// available up front via cheap metadata probes — a GLB's keyframe times are declared in its JSON
// chunk (accessor max / last input key), a .af's frame count sits in its 64-byte header — so the
// scene browser can show loop lengths without ozz-decoding a thousand files.
//
// Values live in <Documents>\My Games\Starfield\OSF\clip-durations.json, keyed on the authored
// clip spec (normalized) + animation id, invalidated by loose-file size+mtime. Real decodes
// overwrite probe values (Record), so the two sources converge on the exact number.

#include <functional>
#include <optional>
#include <string_view>

namespace OSF::Serialization::ClipDurations
{
	// Clip loop length in seconds, or nullopt when the clip hasn't been probed or decoded yet
	// (missing file, unprobeable metadata, scan still running). a_fileSpec is the authored spec
	// string exactly as it appears in scene JSON ("naf:" prefix and Data-relative forms both fine).
	std::optional<float> Lookup(std::string_view a_fileSpec, std::string_view a_animId);

	// Record the exact duration of a successfully decoded clip. Persists only on change, so the
	// common case (probe already matched) costs a map lookup.
	void Record(std::string_view a_fileSpec, std::string_view a_animId, float a_seconds);

	// Probe every clip referenced by the loaded scene registry on a background thread (engine-free
	// file IO only), refresh stale cache entries, and persist. a_onChanged runs on the GAME MAIN
	// THREAD (SFSE task) when at least one duration changed — the UI catalog re-push hook.
	// A no-op if a scan is already running.
	void ScanSceneClipsAsync(std::function<void()> a_onChanged);
}
