#pragma once

// Scenes match by tags + a complete per-role filter binding, ranked by priority tier then weighted-random.
// Binding is deterministic complete matching (not greedy) and travels with the candidate.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OSF::Registry
{
	struct SceneRole;
	struct SceneDef;
}

namespace OSF::Animation { enum class WorldPlacementMode : std::uint8_t; }

namespace OSF::Matchmaking
{
	// kAllow: free scenes always; anchor-bound only if a_anchor satisfies them (null => free only).
	// kRequire: only anchor-bound scenes a_anchor satisfies.
	enum class AnchorMode : std::uint8_t
	{
		kAllow,
		kRequire
	};

	// Empty anyOf is ignored.
	struct TagQuery
	{
		std::vector<std::string> allOf;
		std::vector<std::string> anyOf;
		std::vector<std::string> noneOf;
	};

	struct Candidate
	{
		std::string              id;
		std::int32_t             priority = 0;
		std::int32_t             weight = 1;
		// role index -> index into the caller's actor array
		std::vector<std::size_t> order;
	};

	// Gender + any-of keyword + any-of race. Shared with SceneRuntime's start-path enforcement.
	bool RoleAccepts(const Registry::SceneRole& a_role, RE::Actor* a_actor);

	// Empty role names keep caller order; otherwise every actor must name exactly one role.
	bool BindSceneRoles(const Registry::SceneDef& a_def, const std::vector<RE::Actor*>& a_actors,
		const std::vector<std::string>& a_roleNames, std::vector<RE::Actor*>& a_ordered, std::string& a_error);

	// Any-of: listed base form or listed keyword. a_matchedKeyword gets the matching keyword (null on a base-form match).
	bool AnchorAccepts(const Registry::SceneDef& a_def, RE::TESObjectREFR* a_ref, RE::BGSKeyword** a_matchedKeyword = nullptr);

	// Memoizes HasKeyword per unique keyword so a sweep over many defs costs one engine call per distinct keyword.
	class AnchorMatchCache
	{
	public:
		explicit AnchorMatchCache(RE::TESObjectREFR* a_ref);

		// False when constructed with a null ref.
		bool Accepts(const Registry::SceneDef& a_def);

	private:
		RE::TESObjectREFR*                       ref = nullptr;
		RE::TESFormID                            baseId = 0;
		std::unordered_map<RE::TESFormID, bool>  kwHits;  // keyword FormID -> HasKeyword result
	};

	// "male" / "female", or "" for kNone/unknown.
	std::string ActorGenderTag(RE::Actor* a_actor);

	// Highest priority tier, weighted-random within it. nullopt = no fit.
	std::optional<Candidate> Pick(const std::vector<RE::Actor*>& a_actors, const TagQuery& a_query, RE::TESObjectREFR* a_anchor = nullptr, AnchorMode a_mode = AnchorMode::kAllow);
	std::optional<Candidate> PickReplacement(const std::vector<RE::Actor*>& a_actors, const TagQuery& a_query,
		RE::TESObjectREFR* a_anchor, std::string_view a_exclude, Animation::WorldPlacementMode a_placement);
}
