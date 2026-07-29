#pragma once

// Tag/role matchmaking over the scene registry: a scene is matched by tags + a complete per-role filter binding (gender + any-of keyword + any-of race), ranked by priority tier then weighted-random.
// Role binding uses deterministic COMPLETE matching (not greedy); the chosen candidate carries its resolved binding so the start path never re-derives it. 

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

namespace OSF::Matchmaking
{
	// How matchmaking treats a scene's `anchor` requirement (an anchor-bound scene, one with a keyword/base matcher, vs a free scene). a_anchor is the placed ref the scene would anchor to.
	//   kAllow   : a START with an OPTIONAL anchor — free scenes always eligible; an anchor-bound scene only if a_anchor satisfies its matcher. a_anchor null => only free scenes.
	//   kRequire : anchor-FIRST — ONLY anchor-bound scenes a_anchor satisfies (free scenes excluded).
	enum class AnchorMode : std::uint8_t
	{
		kAllow,
		kRequire
	};

	// Boolean tag query. allOf: every tag must be present; anyOf: at least one (empty = ignored); noneOf: none may be present. (A tag in both allOf and noneOf simply never matches.)
	// The plain all-of form (StartSceneByTags) passes its tags as allOf with empty any/none.
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
		// slot/role index -> index into the caller's actor array (the resolved binding).
		std::vector<std::size_t> order;
	};

	// Does a_actor satisfy a_role's filters (gender + any-of keyword + any-of race)? shared by matchmaking and SceneRuntime's per-start filter enforcement.
	bool RoleAccepts(const Registry::SceneRole& a_role, RE::Actor* a_actor);

	// Does a_ref satisfy a_def's anchor requirement (any-of: its base form is listed, OR it carries a listed keyword)? Shared by anchor-first matchmaking and the start-path anchor enforcement.
	// a_matchedKeyword (optional) receives the keyword that matched — null on a base-form match (Scan Nearby labels unnamed markers by it).
	bool AnchorAccepts(const Registry::SceneDef& a_def, RE::TESObjectREFR* a_ref, RE::BGSKeyword** a_matchedKeyword = nullptr);

	// AnchorAccepts for ONE fixed ref swept against MANY defs (pool build, anchor-match queries):
	// memoizes ref->HasKeyword per UNIQUE keyword and resolves the ref's base form once, so the sweep costs one engine call per distinct keyword instead of per (def x keyword).
	class AnchorMatchCache
	{
	public:
		explicit AnchorMatchCache(RE::TESObjectREFR* a_ref);

		// Same predicate as AnchorAccepts(a_def, ref); false when constructed with a null ref.
		bool Accepts(const Registry::SceneDef& a_def);

	private:
		RE::TESObjectREFR*                       ref = nullptr;
		RE::TESFormID                            baseId = 0;
		std::unordered_map<RE::TESFormID, bool>  kwHits;  // keyword FormID -> ref->HasKeyword result
	};

	// gender as a lowercase tag: "male" / "female", or "" for kNone/unknown (creature, no actorbase).
	std::string ActorGenderTag(RE::Actor* a_actor);

	// Pick one candidate for StartSceneByTags* / StartSceneAtAnchor: build the pool (filter-aware), take the highest priority tier, weighted-random within it.
	// The result carries its binding (Candidate::order). a_anchor + a_mode add anchor filtering (see AnchorMode). nullopt = no fit.
	std::optional<Candidate> Pick(const std::vector<RE::Actor*>& a_actors, const TagQuery& a_query, RE::TESObjectREFR* a_anchor = nullptr, AnchorMode a_mode = AnchorMode::kAllow);
}
