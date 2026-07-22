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
		auto rel = path.lexically_relative(data);
		if (rel.empty() || rel.string().starts_with("..")) {
			return std::nullopt;
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

	ClipSpec ResolveClipSpec(const std::filesystem::path& a_spec)
	{
		ClipSpec out;
		out.display = ClipSpecDisplay(a_spec);
		const std::string raw = a_spec.string();

		if (ToLower(raw).starts_with("naf:")) {
			out.candidates.push_back({ out.display, std::filesystem::current_path() / "Data" / out.display, true });
			return out;
		}

		if (a_spec.is_absolute()) {
			if (auto rel = DataRelativePath(a_spec)) {
				out.candidates.push_back({ *rel, std::filesystem::current_path() / "Data" / *rel, true });
			} else {
				out.candidates.push_back({ {}, a_spec, false });
			}
			return out;
		}

		const std::string& primary = out.display;
		out.candidates.push_back({ primary, std::filesystem::current_path() / "Data" / primary, true });
		out.candidates.push_back({ NormalizeResourcePath(std::string{ "NAF\\" } + primary),
			std::filesystem::current_path() / "Data" / "NAF" / primary, true });
		out.candidates.push_back({ NormalizeResourcePath(std::string{ "OSF\\Animations\\" } + a_spec.filename().string()),
			std::filesystem::current_path() / "Data" / "OSF" / "Animations" / a_spec.filename(), true });
		return out;
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
