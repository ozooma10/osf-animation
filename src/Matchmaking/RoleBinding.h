#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace OSF::Matchmaking
{
	// Pure permutation validation; actor eligibility is supplied by the caller.
	template <class Actor, class Accepts>
	bool BindRoleNames(const std::vector<std::string>& declared, const std::vector<Actor*>& actors,
		const std::vector<std::string>& names, std::vector<Actor*>& ordered, std::string& error, Accepts accepts)
	{
		ordered.clear();
		if (actors.size() != declared.size() || (!names.empty() && names.size() != actors.size())) {
			error = "Cast and role name counts must match the scene";
			return false;
		}
		const auto lower = [](std::string value) {
			for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return value;
		};
		std::vector<Actor*> result(declared.size());
		for (std::size_t i = 0; i < actors.size(); ++i) {
			std::size_t role = i;
			if (!names.empty()) {
				const auto it = std::ranges::find_if(declared, [&](const auto& name) { return lower(name) == lower(names[i]); });
				if (it == declared.end()) { error = "Unknown role '" + names[i] + "'"; return false; }
				role = static_cast<std::size_t>(it - declared.begin());
			}
			if (!actors[i] || result[role] || std::ranges::find(result, actors[i]) != result.end()) {
				error = "Cast contains a missing actor, duplicate actor or duplicate role";
				return false;
			}
			if (!accepts(role, actors[i])) { error = "Actor does not satisfy role '" + declared[role] + "'"; return false; }
			result[role] = actors[i];
		}
		ordered = std::move(result);
		return true;
	}
}
