#include "Matchmaking/Matchmaker.h"

#include "Registry/SceneRegistry.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <format>
#include <random>
#include <unordered_set>

namespace OSF::Matchmaking
{
	using OSF::Util::ToLower;

	namespace
	{
		RE::SEX SexOf(RE::Actor* a_actor)
		{
			auto* npc = a_actor ? a_actor->GetNPC() : nullptr;
			return npc ? npc->GetSex() : RE::SEX::kNone;
		}

		// kAny accepts anyone (including a kNone-sex actor); a gendered slot needs the matching sex.
		bool SlotGenderAccepts(Registry::SlotGender a_slot, RE::SEX a_sex)
		{
			using G = Registry::SlotGender;
			if (a_slot == G::kMale) {
				return a_sex == RE::SEX::kMale;
			}
			if (a_slot == G::kFemale) {
				return a_sex == RE::SEX::kFemale;
			}
			return true;
		}

		// The actor "has" a keyword if its actorbase (TESNPC, a BGSKeywordForm) OR its race (also a
		// BGSKeywordForm) carries it, matching the game's actor keyword condition reasonably.
		bool ActorHasKeyword(RE::Actor* a_actor, RE::BGSKeyword* a_keyword)
		{
			if (!a_actor || !a_keyword) {
				return false;
			}
			// Qualify the base call: TESNPC declares its own HasKeyword(string_view), which hides the inherited BGSKeywordForm::HasKeyword(BGSKeyword*). 
			// The base checks the actorbase's own keywords; the race is checked separately so an actor matches if EITHER carries it.
			if (auto* npc = a_actor->GetNPC(); npc && npc->BGSKeywordForm::HasKeyword(a_keyword)) {
				return true;
			}
			if (auto* race = a_actor->race; race && race->BGSKeywordForm::HasKeyword(a_keyword)) {
				return true;
			}
			return false;
		}

		std::vector<std::string> Lower(const std::vector<std::string>& a_in)
		{
			std::vector<std::string> out;
			out.reserve(a_in.size());
			for (const auto& s : a_in) {
				out.push_back(ToLower(s));
			}
			return out;
		}

		// a_query is already lowercased. a_defTags is raw (lowered here).
		bool TagsMatch(const std::vector<std::string>& a_defTags, const TagQuery& a_query)
		{
			std::unordered_set<std::string> have;
			have.reserve(a_defTags.size());
			for (const auto& t : a_defTags) {
				have.insert(ToLower(t));
			}
			for (const auto& t : a_query.allOf) {
				if (!have.count(t)) {
					return false;
				}
			}
			if (!a_query.anyOf.empty()) {
				bool any = false;
				for (const auto& t : a_query.anyOf) {
					if (have.count(t)) {
						any = true;
						break;
					}
				}
				if (!any) {
					return false;
				}
			}
			for (const auto& t : a_query.noneOf) {
				if (have.count(t)) {
					return false;
				}
			}
			return true;
		}

		// Deterministic complete assignment of a_n actors to a_n slots: perm[slot] = actor index.
		// Iterates permutations in lexicographic order and returns the first one where every slot is compatible (so the result is the lexicographically smallest binding by slot order). 
		// nullopt if no complete assignment exists. a_n is small (party size), so brute force is fine, this is what the pack matcher already does.
		template <class Pred>
		std::optional<std::vector<std::size_t>> MatchComplete(std::size_t a_n, Pred a_compatible)
		{
			std::vector<std::size_t> perm(a_n);
			for (std::size_t i = 0; i < a_n; i++) {
				perm[i] = i;
			}
			do {
				bool ok = true;
				for (std::size_t slot = 0; slot < a_n; slot++) {
					if (!a_compatible(slot, perm[slot])) {
						ok = false;
						break;
					}
				}
				if (ok) {
					return perm;
				}
			} while (std::next_permutation(perm.begin(), perm.end()));
			return std::nullopt;
		}

		// Build the unified candidate pool. a_actors non-empty => filter-aware with a complete binding (Candidate::order filled);
		// empty => count + tags only (discovery, no binding). a_anchor + a_mode add anchor filtering (see AnchorMode;
		// kIgnore = no filter, the discovery default).
		std::vector<Candidate> BuildPool(std::int32_t a_count, const TagQuery& a_query, const std::vector<RE::Actor*>& a_actors,
			RE::TESObjectREFR* a_anchor = nullptr, AnchorMode a_mode = AnchorMode::kIgnore)
		{
			const bool haveActors = !a_actors.empty();
			const TagQuery q{ Lower(a_query.allOf), Lower(a_query.anyOf), Lower(a_query.noneOf) };

			std::vector<Candidate> pool;

			Registry::SceneRegistry::GetSingleton().ForEachDef([&](const Registry::SceneDef& a_def) {
				if (static_cast<std::int32_t>(a_def.roles.size()) != a_count) {
					return;
				}
				if (!TagsMatch(a_def.tags, q)) {
					return;
				}
				// Anchor filtering: an anchor-bound scene needs a ref that satisfies it; kRequire (anchor-first)
				// additionally drops free scenes. kIgnore (discovery) skips this entirely.
				if (a_mode != AnchorMode::kIgnore) {
					if (a_def.RequiresAnchor()) {
						if (!a_anchor || !AnchorAccepts(a_def, a_anchor)) {
							return;
						}
					} else if (a_mode == AnchorMode::kRequire) {
						return;
					}
				}
				Candidate c;
				c.id = a_def.id;
				c.priority = a_def.priority;
				c.weight = a_def.weight;
				if (haveActors) {
					auto order = MatchComplete(static_cast<std::size_t>(a_count),
						[&](std::size_t a_slot, std::size_t a_ai) { return RoleAccepts(a_def.roles[a_slot], a_actors[a_ai]); });
					if (!order) {
						return;
					}
					c.order = std::move(*order);
				}
				pool.push_back(std::move(c));
			});

			return pool;
		}

		// Compact, log-friendly rendering of a tag query, e.g. "all=[a,b] any=[c] none=[d]".
		std::string DescribeQuery(const TagQuery& a_query)
		{
			auto join = [](const std::vector<std::string>& a_in) {
				std::string out;
				for (std::size_t i = 0; i < a_in.size(); i++) {
					if (i) {
						out += ',';
					}
					out += a_in[i];
				}
				return out;
			};
			return std::format("all=[{}] any=[{}] none=[{}]", join(a_query.allOf), join(a_query.anyOf), join(a_query.noneOf));
		}
	}

	bool RoleAccepts(const Registry::SceneRole& a_role, RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		if (!SlotGenderAccepts(a_role.gender, SexOf(a_actor))) {
			return false;
		}
		if (!a_role.keywords.empty()) {
			bool ok = false;
			for (auto* kw : a_role.keywords) {
				if (ActorHasKeyword(a_actor, kw)) {
					ok = true;
					break;
				}
			}
			if (!ok) {
				return false;
			}
		}
		if (!a_role.races.empty()) {
			bool ok = false;
			auto* race = a_actor->race;
			for (auto* rc : a_role.races) {
				if (race && race == rc) {
					ok = true;
					break;
				}
			}
			if (!ok) {
				return false;
			}
		}
		return true;
	}

	bool AnchorAccepts(const Registry::SceneDef& a_def, RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return false;
		}
		if (auto base = a_ref->GetBaseObject()) {
			const auto id = base->GetFormID();
			for (const auto b : a_def.anchorBaseForms) {
				if (b == id) {
					return true;
				}
			}
		}
		for (auto* kw : a_def.anchorKeywords) {
			if (kw && a_ref->HasKeyword(kw)) {
				return true;
			}
		}
		return false;
	}

	std::string ActorGenderTag(RE::Actor* a_actor)
	{
		switch (SexOf(a_actor)) {
		case RE::SEX::kMale:
			return "male";
		case RE::SEX::kFemale:
			return "female";
		default:
			return {};  // kNone / no actorbase -> gender-agnostic (the {gender} tag drops out)
		}
	}

	std::vector<std::string> FindIds(std::int32_t a_actorCount, const TagQuery& a_query,
		const std::vector<RE::Actor*>& a_actors)
	{
		auto pool = BuildPool(a_actorCount, a_query, a_actors);
		std::sort(pool.begin(), pool.end(), [](const Candidate& a_lhs, const Candidate& a_rhs) {
			if (a_lhs.priority != a_rhs.priority) {
				return a_lhs.priority > a_rhs.priority;  // priority descending
			}
			return ToLower(a_lhs.id) < ToLower(a_rhs.id);  // then id ascending (stable, case-insensitive)
		});
		std::vector<std::string> ids;
		ids.reserve(pool.size());
		for (auto& c : pool) {
			ids.push_back(std::move(c.id));
		}
		return ids;
	}

	std::optional<Candidate> Pick(const std::vector<RE::Actor*>& a_actors, const TagQuery& a_query,
		RE::TESObjectREFR* a_anchor, AnchorMode a_mode)
	{
		const auto actorCount = static_cast<std::int32_t>(a_actors.size());
		REX::DEBUG("[Match] searching for {} actor(s), tags {}", actorCount, DescribeQuery(a_query));
		if (a_actors.empty()) {
			REX::DEBUG("[Match] no match (no actors supplied)");
			return std::nullopt;
		}
		auto pool = BuildPool(actorCount, a_query, a_actors, a_anchor, a_mode);
		if (pool.empty()) {
			REX::DEBUG("[Match] no match (no scene def fit {} role(s) + tags + a complete actor binding)", actorCount);
			return std::nullopt;
		}
		// Top priority tier.
		std::int32_t maxPriority = pool.front().priority;
		for (const auto& c : pool) {
			maxPriority = std::max(maxPriority, c.priority);
		}
		std::vector<const Candidate*> tier;
		std::uint64_t total = 0;
		for (const auto& c : pool) {
			if (c.priority == maxPriority) {
				tier.push_back(&c);
				total += static_cast<std::uint64_t>(std::max(1, c.weight));
			}
		}
		REX::TRACE("[Match] {} candidate(s) in pool, {} in top priority tier {} (total weight {})",
			pool.size(), tier.size(), maxPriority, total);
		// Weight-proportional random within the tier (uint64 sum is overflow-safe for the [1,1e6] cap).
		std::mt19937 rng{ std::random_device{}() };
		std::uniform_int_distribution<std::uint64_t> dist(1, total);
		auto roll = dist(rng);
		REX::TRACE("[Match] weighted roll {} of {}", roll, total);
		for (const auto* c : tier) {
			const auto w = static_cast<std::uint64_t>(std::max(1, c->weight));
			if (roll <= w) {
				REX::DEBUG("[Match] chose '{}' (priority {}, weight {})", c->id, c->priority, c->weight);
				return *c;
			}
			roll -= w;
		}
		REX::DEBUG("[Match] chose '{}' (priority {}, weight {}) [tail guard]",
			tier.back()->id, tier.back()->priority, tier.back()->weight);
		return *tier.back();  // numerical guard; not normally reached
	}
}
