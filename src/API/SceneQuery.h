#pragma once

// Converts the native OSFSceneQuery / OSFSceneMatch ABI to and from matchmaking types. No engine calls.

#include "API/OSFSceneAPI.h"
#include "Matchmaking/Matchmaker.h"

#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace OSF::API
{
	inline constexpr std::size_t kSceneQueryV1Size = sizeof(OSFSceneQuery);
	inline constexpr std::uint32_t kSceneQueryMaxEntries = 256;

	struct SceneQueryInput
	{
		Matchmaking::TagQuery        tags;
		Matchmaking::RoleConstraints roles;
		RE::TESObjectREFR*           anchor = nullptr;
		Matchmaking::AnchorMode      mode = Matchmaking::AnchorMode::kAllow;
	};

	// nullopt with a_error set on malformed input. a_actorCount is the cast size the role constraints must match.
	inline std::optional<SceneQueryInput> ReadSceneQuery(const OSFSceneQuery& a_query, std::size_t a_actorCount, std::string& a_error)
	{
		if (a_query.size < kSceneQueryV1Size) {
			a_error = "OSFSceneQuery size " + std::to_string(a_query.size) + " is smaller than " + std::to_string(kSceneQueryV1Size);
			return std::nullopt;
		}
		const auto list = [&](const char* const* a_items, std::uint32_t a_count, const char* a_name,
							  std::vector<std::string>& a_out, bool a_keepEmpty) {
			if (a_count > kSceneQueryMaxEntries || (a_count && !a_items)) {
				a_error = std::string(a_name) + " has a null array or more than 256 entries";
				return false;
			}
			for (std::uint32_t i = 0; i < a_count; ++i) {
				const char* item = a_items[i];
				if (item && *item) a_out.emplace_back(item);
				else if (a_keepEmpty) a_out.emplace_back();
			}
			return true;
		};
		SceneQueryInput in;
		if (!list(a_query.allOf, a_query.allOfCount, "allOf", in.tags.allOf, false) ||
			!list(a_query.anyOf, a_query.anyOfCount, "anyOf", in.tags.anyOf, false) ||
			!list(a_query.noneOf, a_query.noneOfCount, "noneOf", in.tags.noneOf, false) ||
			!list(a_query.roles, a_query.roleCount, "roles", in.roles, true)) {
			return std::nullopt;
		}
		if (!Matchmaking::RoleConstraintsFit(in.roles, a_actorCount)) {
			a_error = std::to_string(in.roles.size()) + " role constraint(s) for " + std::to_string(a_actorCount) + " actor(s)";
			return std::nullopt;
		}
		if (a_query.requireAnchor == 1 && !a_query.anchor) {
			a_error = "requireAnchor needs an anchor";
			return std::nullopt;
		}
		in.anchor = a_query.anchor;
		in.mode = a_query.requireAnchor == 1 ? Matchmaking::AnchorMode::kRequire : Matchmaking::AnchorMode::kAllow;
		return in;
	}

	// a_declared = the picked scene's role names in declaration order. False when the result cannot be represented.
	inline bool WriteSceneMatch(const Matchmaking::Candidate& a_pick, const std::vector<std::string>& a_declared,
		OSFSceneMatch& a_out, std::string& a_error)
	{
		const auto count = a_pick.order.size();
		const auto roleOf = Matchmaking::RoleOfActor(a_pick.order, count);
		if (count > kOSFSceneMatchMaxActors || a_declared.size() != count || roleOf.empty() && count) {
			a_error = "match binding is not a permutation of at most 8 actors";
			return false;
		}
		if (a_pick.id.size() >= sizeof(a_out.nodeId)) {
			a_error = "scene id '" + a_pick.id + "' is longer than 255 bytes";
			return false;
		}
		for (const auto& name : a_declared) {
			if (name.size() >= sizeof(a_out.roles[0])) {
				a_error = "role name '" + name + "' is longer than 63 bytes";
				return false;
			}
		}
		const auto size = a_out.size;
		std::memset(&a_out, 0, sizeof(a_out));
		a_out.size = size;
		a_out.actorCount = static_cast<std::uint32_t>(count);
		std::memcpy(a_out.nodeId, a_pick.id.c_str(), a_pick.id.size() + 1);
		for (std::size_t i = 0; i < count; ++i) {
			const auto& name = a_declared[roleOf[i]];
			a_out.roleIndex[i] = static_cast<std::int32_t>(roleOf[i]);
			std::memcpy(a_out.roles[i], name.c_str(), name.size() + 1);
		}
		return true;
	}
}
