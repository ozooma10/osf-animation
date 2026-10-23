#pragma once

#include <filesystem>
#include <functional>
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

	// Candidates in precedence order: "naf:X" -> Data\NAF\X only; absolute as-is; relative tries
	// the path, then Data\NAF\<path>, then Data\OSF\Animations\<filename>.
	ClipSpec ResolveClipSpec(const std::filesystem::path& a_spec);

	// Same precedence as ResolveClipSpec, stopping at the first successful probe. Resource probes
	// receive only the mounted path; unused fallbacks and their full filesystem paths are not built.
	bool ProbeClipSpec(const std::filesystem::path& a_spec,
		const std::function<bool(std::string_view)>& a_resource,
		const std::function<bool(const std::filesystem::path&)>& a_file);

	// ResolveClipSpec().display without touching the filesystem; the clip-duration cache key.
	std::string ClipSpecDisplay(const std::filesystem::path& a_spec);

	// "File.glb:animationId"; colons in non-glTF paths (drive prefixes) stay in the path.
	std::pair<std::string, std::string> SplitRuntimeClipSpec(std::string a_spec);
}
