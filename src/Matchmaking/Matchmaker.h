#pragma once

// Tag/role matchmaking over the scene registry: a scene is matched by tags + a complete per-role filter binding (gender + any-of keyword + any-of race), ranked by priority tier then weighted-random.
// Role binding uses deterministic COMPLETE matching (not greedy); the chosen candidate carries its resolved binding so the start path never re-derives it. 

#include <optional>
#include <string>
#include <vector>

namespace OSF::Registry
{
	struct SceneRole;
}

namespace OSF::Matchmaking
{
	// Boolean tag query. allOf: every tag must be present; anyOf: at least one (empty = ignored); noneOf: none may be present. (A tag in both allOf and noneOf simply never matches.) 
	// The plain all-of forms (StartSceneByTags / FindScenes) pass their tags as allOf with empty any/none.
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
		// Empty on the actor-less discovery path.
		std::vector<std::size_t> order;
	};

	// Does a_actor satisfy a_role's filters (gender + any-of keyword + any-of race)? shared by matchmaking and SceneRuntime's per-start filter enforcement.
	bool RoleAccepts(const Registry::SceneRole& a_role, RE::Actor* a_actor);

	// gender as a lowercase tag: "male" / "female", or "" for kNone/unknown (creature, no actorbase).
	std::string ActorGenderTag(RE::Actor* a_actor);

	// Deterministic candidate id list for FindScenes / FindScenesForActorsQuery: priority descending, then id ascending. 
	// If a_actors is empty this is the filter-UNAWARE discovery path (a_actorCount + tags only, no binding); 
	// otherwise a scene def appears only if a complete filter-satisfying binding exists. 
	// Pack candidates require a complete gender-slot binding when actors are supplied.
	std::vector<std::string> FindIds(std::int32_t a_actorCount, const TagQuery& a_query, const std::vector<RE::Actor*>& a_actors);

	// Pick one candidate for StartSceneByTags* : build the pool (filter-aware), take the highest priority tier, weighted-random within it. 
	// The result carries its binding (Candidate::order). nullopt = no fit.
	std::optional<Candidate> Pick(const std::vector<RE::Actor*>& a_actors, const TagQuery& a_query);
}
