#pragma once

// Clip file-spec resolution shared by playback (GraphManager::LoadClip) and the clip-duration
// scanner (Serialization::ClipDurations). One source of truth for how an authored clip string
// ("naf:...", Data-relative, absolute) maps onto resource paths + loose-file fallbacks.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OSF::Util
{
	// Engine-resource path form: backslashes, no leading slashes, no "Data\" prefix.
	std::string NormalizeResourcePath(std::string_view a_path);

	// a_path rewritten relative to <cwd>\Data in resource form, or nullopt when it points outside Data.
	std::optional<std::string> DataRelativePath(const std::filesystem::path& a_path);

	struct ClipCandidate
	{
		std::string           resourcePath;
		std::filesystem::path filePath;
		bool                  resource = true;
	};

	struct ClipSpec
	{
		std::string                display;
		std::vector<ClipCandidate> candidates;
	};

	// Expands an authored clip spec into its lookup candidates, in engine precedence order:
	// "naf:X" -> Data\NAF\X only; absolute paths as-is; relative paths try the path itself,
	// then Data\NAF\<path>, then Data\OSF\Animations\<filename>.
	ClipSpec ResolveClipSpec(const std::filesystem::path& a_spec);

	// The display/identity form of an authored spec — ResolveClipSpec().display without building
	// candidates or touching the filesystem (string-only, no syscalls, nothing to throw beyond
	// allocation). The clip-duration cache keys on this.
	std::string ClipSpecDisplay(const std::filesystem::path& a_spec);

	// Runtime glTF selector syntax: "File.glb:animationId". Colons in non-glTF paths (for
	// example a Windows drive prefix) remain part of the path.
	std::pair<std::string, std::string> SplitRuntimeClipSpec(std::string a_spec);
}
