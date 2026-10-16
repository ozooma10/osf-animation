#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::Matchmaking
{
	// Per-actor role-name constraints, aligned with the cast. Empty = unconstrained; an empty entry accepts any role.
	using RoleConstraints = std::vector<std::string>;

	// Case-insensitive; an empty constraint accepts every role, including anonymous ones.
	inline bool RoleNameMatches(std::string_view a_constraint, std::string_view a_roleName)
	{
		return a_constraint.empty() || std::ranges::equal(a_constraint, a_roleName, [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		});
	}

	inline bool RoleConstraintsFit(const RoleConstraints& a_roles, std::size_t a_actorCount)
	{
		return a_roles.empty() || a_roles.size() == a_actorCount;
	}

	// order[role] = actor index: the lexicographically first assignment where every pair is compatible, so cast
	// order breaks ties. Brute force is fine at party size.
	template <class Compatible>
	std::optional<std::vector<std::size_t>> FindRoleOrder(std::size_t a_count, Compatible a_compatible)
	{
		std::vector<std::size_t> order(a_count);
		for (std::size_t i = 0; i < a_count; ++i) order[i] = i;
		do {
			bool ok = true;
			for (std::size_t role = 0; role < a_count && ok; ++role) ok = a_compatible(role, order[role]);
			if (ok) return order;
		} while (std::next_permutation(order.begin(), order.end()));
		return std::nullopt;
	}

	// Name binding is unambiguous only when no two declared roles share a name (case-insensitive; "" counts as a name).
	inline bool RoleNamesUnique(const std::vector<std::string>& a_names)
	{
		const auto same = [](const std::string& a, const std::string& b) {
			return std::ranges::equal(a, b, [](char x, char y) {
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			});
		};
		for (std::size_t i = 0; i < a_names.size(); ++i)
			for (std::size_t j = i + 1; j < a_names.size(); ++j)
				if (same(a_names[i], a_names[j])) return false;
		return true;
	}

	// Inverts order[role] = actor into roleOf[actor] = role. Empty when order is not a permutation of a_count.
	inline std::vector<std::size_t> RoleOfActor(const std::vector<std::size_t>& a_order, std::size_t a_count)
	{
		if (a_order.size() != a_count) return {};
		std::vector<std::size_t> roleOf(a_count, a_count);
		for (std::size_t role = 0; role < a_count; ++role) {
			const auto actor = a_order[role];
			if (actor >= a_count || roleOf[actor] != a_count) return {};
			roleOf[actor] = role;
		}
		return roleOf;
	}

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
