#include "Util/ClipPath.h"

#include "Util/StringUtil.h"

#include <algorithm>

namespace OSF::Util
{
	std::string NormalizeResourcePath(std::string_view a_path)
	{
		std::string s{ a_path };
		std::replace(s.begin(), s.end(), '/', '\\');
		while (!s.empty() && (s.front() == '\\' || s.front() == '/')) {
			s.erase(s.begin());
		}

		const auto lower = ToLower(s);
		if (lower.starts_with("data\\")) {
			s.erase(0, 5);
		}
		return s;
	}

	std::optional<std::string> DataRelativePath(const std::filesystem::path& a_path)
	{
		if (!a_path.is_absolute()) {
			return NormalizeResourcePath(a_path.string());
		}

		auto data = (std::filesystem::current_path() / "Data").lexically_normal();
		auto path = a_path.lexically_normal();
		auto dataIt = data.begin();
		auto pathIt = path.begin();
		for (; dataIt != data.end(); ++dataIt, ++pathIt) {
			if (pathIt == path.end() ||
				ToLower(dataIt->generic_string()) != ToLower(pathIt->generic_string())) {
				return std::nullopt;
			}
		}
		if (pathIt == path.end()) {
			return std::nullopt;
		}

		std::filesystem::path rel;
		for (; pathIt != path.end(); ++pathIt) {
			rel /= *pathIt;
		}
		return NormalizeResourcePath(rel.string());
	}

	std::string ClipSpecDisplay(const std::filesystem::path& a_spec)
	{
		const std::string raw = a_spec.string();
		if (ToLower(raw).starts_with("naf:")) {
			return NormalizeResourcePath(std::string{ "NAF\\" } + raw.substr(4));
		}
		if (a_spec.is_absolute()) {
			return raw;
		}
		return NormalizeResourcePath(raw);
	}

	namespace
	{
		// Both materialized playback candidates and short-circuit availability checks use these
		// rules. The resource callback chooses whether it needs the full-file-path factory.
		template <class Resource, class File>
		bool VisitCandidates(const std::filesystem::path& a_spec, const std::string& a_display,
			Resource&& a_resource, File&& a_file)
		{
			const std::string raw = a_spec.string();
			std::optional<std::filesystem::path> data;
			const auto dataRoot = [&]() -> const std::filesystem::path& {
				if (!data) data = std::filesystem::current_path() / "Data";
				return *data;
			};

			if (ToLower(raw).starts_with("naf:")) {
				return a_resource(a_display, [&] { return dataRoot() / a_display; });
			}

			if (a_spec.is_absolute()) {
				if (auto rel = DataRelativePath(a_spec)) {
					return a_resource(*rel, [&] { return dataRoot() / *rel; });
				}
				return a_file(a_spec);
			}

			if (a_resource(a_display, [&] { return dataRoot() / a_display; })) return true;
			const auto naf = NormalizeResourcePath(std::string{ "NAF\\" } + a_display);
			if (a_resource(naf, [&] { return dataRoot() / "NAF" / a_display; })) return true;
			const auto filename = a_spec.filename();
			return a_resource(NormalizeResourcePath(std::string{ "OSF\\Animations\\" } + filename.string()),
				[&] { return dataRoot() / "OSF" / "Animations" / filename; });
		}
	}

	ClipSpec ResolveClipSpec(const std::filesystem::path& a_spec)
	{
		ClipSpec out;
		out.display = ClipSpecDisplay(a_spec);
		VisitCandidates(a_spec, out.display,
			[&](const std::string& resource, auto&& filePath) {
				out.candidates.push_back({resource, filePath(), true});
				return false;
			},
			[&](const std::filesystem::path& file) {
				out.candidates.push_back({{}, file, false});
				return false;
			});
		return out;
	}

	bool ProbeClipSpec(const std::filesystem::path& a_spec,
		const std::function<bool(std::string_view)>& a_resource,
		const std::function<bool(const std::filesystem::path&)>& a_file)
	{
		return VisitCandidates(a_spec, ClipSpecDisplay(a_spec),
			[&](const std::string& resource, auto&&) { return a_resource(resource); }, a_file);
	}

	std::pair<std::string, std::string> SplitRuntimeClipSpec(std::string a_spec)
	{
		const auto pos = a_spec.rfind(':');
		if (pos == std::string::npos || pos + 1 >= a_spec.size()) {
			return { std::move(a_spec), {} };
		}
		std::string pathPart = a_spec.substr(0, pos);
		const auto ext = ToLower(std::filesystem::path{ pathPart }.extension().string());
		if (ext != ".glb" && ext != ".gltf") {
			return { std::move(a_spec), {} };
		}
		std::string animId = a_spec.substr(pos + 1);
		return { std::move(pathPart), std::move(animId) };
	}
}
