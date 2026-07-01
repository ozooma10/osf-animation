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

	ClipSpec ResolveClipSpec(const std::filesystem::path& a_spec)
	{
		ClipSpec out;
		const std::string raw = a_spec.string();
		if (ToLower(raw).starts_with("naf:")) {
			auto rel = NormalizeResourcePath(std::string{ "NAF\\" } + raw.substr(4));
			out.display = rel;
			out.candidates.push_back({ rel, std::filesystem::current_path() / "Data" / rel, true });
			return out;
		}

		if (a_spec.is_absolute()) {
			out.display = a_spec.string();
			if (auto rel = DataRelativePath(a_spec)) {
				out.candidates.push_back({ *rel, std::filesystem::current_path() / "Data" / *rel, true });
			} else {
				out.candidates.push_back({ {}, a_spec, false });
			}
			return out;
		}

		auto primary = NormalizeResourcePath(raw);
		out.display = primary;
		out.candidates.push_back({ primary, std::filesystem::current_path() / "Data" / primary, true });
		out.candidates.push_back({ NormalizeResourcePath(std::string{ "NAF\\" } + primary),
			std::filesystem::current_path() / "Data" / "NAF" / primary, true });
		out.candidates.push_back({ NormalizeResourcePath(std::string{ "OSF\\Animations\\" } + a_spec.filename().string()),
			std::filesystem::current_path() / "Data" / "OSF" / "Animations" / a_spec.filename(), true });
		return out;
	}
}
