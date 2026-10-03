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
		RE::TESNPC* NpcOf(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return nullptr;
			}
			const auto base = a_actor->GetBaseObject();
			return base ? base->As<RE::TESNPC>() : nullptr;
		}

		RE::SEX SexOf(RE::Actor* a_actor)
		{
			auto* npc = NpcOf(a_actor);
			return npc ? npc->GetSex() : RE::SEX::kNone;
		}

		bool RoleGenderAccepts(Registry::RoleGender a_roleGender, RE::SEX a_sex)
		{
			using G = Registry::RoleGender;
			if (a_roleGender == G::kMale) {
				return a_sex == RE::SEX::kMale;
			}
			if (a_roleGender == G::kFemale) {
				return a_sex == RE::SEX::kFemale;
			}
			return true;
		}

		// Actorbase or race keywords count.
		bool ActorHasKeyword(RE::Actor* a_actor, RE::BGSKeyword* a_keyword)
		{
			if (!a_actor || !a_keyword) {
				return false;
			}
			// Qualified: TESNPC's HasKeyword(string_view) hides BGSKeywordForm::HasKeyword(BGSKeyword*).
			if (auto* npc = NpcOf(a_actor); npc && npc->BGSKeywordForm::HasKeyword(a_keyword)) {
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

		// a_query is lowercased; a_have is the def's precomputed lowercase SceneDef::tagSet (hot path).
		bool TagsMatch(const std::unordered_set<std::string>& a_have, const TagQuery& a_query)
		{
			for (const auto& t : a_query.allOf) {
				if (!a_have.count(t)) {
					return false;
				}
			}
			if (!a_query.anyOf.empty()) {
				bool any = false;
				for (const auto& t : a_query.anyOf) {
					if (a_have.count(t)) {
						any = true;
						break;
					}
				}
				if (!any) {
					return false;
				}
			}
			for (const auto& t : a_query.noneOf) {
				if (a_have.count(t)) {
					return false;
				}
			}
			return true;
		}

		std::string DescribeAnchorRef(RE::TESObjectREFR* a_ref);

		// perm[role] = actor index; the lexicographically first compatible permutation. Brute force is fine at party size.
		template <class Pred>
		std::optional<std::vector<std::size_t>> MatchComplete(std::size_t a_n, Pred a_compatible)
		{
			std::vector<std::size_t> perm(a_n);
			for (std::size_t i = 0; i < a_n; i++) {
				perm[i] = i;
			}
			do {
				bool ok = true;
				for (std::size_t role = 0; role < a_n; role++) {
					if (!a_compatible(role, perm[role])) {
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

		// Pick guarantees a_actors is non-empty.
		std::vector<Candidate> BuildPool(const TagQuery& a_query, const std::vector<RE::Actor*>& a_actors,
			RE::TESObjectREFR* a_anchor, AnchorMode a_mode)
		{
			const auto count = static_cast<std::int32_t>(a_actors.size());
			const TagQuery q{ Lower(a_query.allOf), Lower(a_query.anyOf), Lower(a_query.noneOf) };

			// Vanilla packs share a handful of AnimFurn* keywords across thousands of defs.
			AnchorMatchCache anchorCache(a_anchor);

			std::vector<Candidate> pool;

			Registry::ContentRegistry::GetSingleton().ForEachDef([&](const Registry::SceneDef& a_def) {
				if (a_def.unlisted || !a_def.clipsAvailable) {
					return;
				}
				if (static_cast<std::int32_t>(a_def.roles.size()) != count) {
					return;
				}
				if (!TagsMatch(a_def.tagSet, q)) {
					return;
				}
				// No per-def logging: REX formats args (engine EditorID calls) before the level check.
				if (a_def.RequiresAnchor()) {
					if (!anchorCache.Accepts(a_def)) {
						return;
					}
				} else if (a_mode == AnchorMode::kRequire) {
					return;
				}
				Candidate c;
				c.id = a_def.id;
				c.priority = a_def.priority;
				c.weight = a_def.weight;
				auto order = MatchComplete(static_cast<std::size_t>(count),
					[&](std::size_t a_role, std::size_t a_actorIndex) {
						return RoleAccepts(a_def.roles[a_role], a_actors[a_actorIndex]);
					});
				if (!order) {
					return;
				}
				c.order = std::move(*order);
				pool.push_back(std::move(c));
			});

			return pool;
		}

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

		const char* DescribeAnchorMode(AnchorMode a_mode)
		{
			switch (a_mode) {
			case AnchorMode::kAllow:
				return "allow";
			case AnchorMode::kRequire:
				return "require";
			default:
				return "?";
			}
		}

		std::string DescribeForm(RE::TESForm* a_form)
		{
			if (!a_form) {
				return "none";
			}
			const char* editorId = a_form->GetFormEditorID();
			return std::format("{:08X} '{}'", a_form->GetFormID(), editorId ? editorId : "");
		}

		std::string DescribeAnchorRef(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return "none";
			}
			auto base = a_ref->GetBaseObject();
			return std::format("ref {} base {}", DescribeForm(a_ref), DescribeForm(base.get()));
		}

		std::string DescribeAnchorSearch(RE::TESObjectREFR* a_anchor, AnchorMode a_mode)
		{
			return std::format("anchorMode={} anchor={}", DescribeAnchorMode(a_mode), DescribeAnchorRef(a_anchor));
		}
	}

	bool RoleAccepts(const Registry::SceneRole& a_role, RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		if (a_role.gender != Registry::RoleGender::kAny && !RoleGenderAccepts(a_role.gender, SexOf(a_actor))) {
			return false;
		}
		if (!a_role.keywords.empty()) {
			bool ok = false;
			for (const auto kwId : a_role.keywords) {
				// The registry stores FormIDs, not pointers.
				if (ActorHasKeyword(a_actor, RE::TESForm::LookupByID<RE::BGSKeyword>(kwId))) {
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
			for (const auto rcId : a_role.races) {
				if (race && race->GetFormID() == rcId) {
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

	bool BindSceneRoles(const Registry::SceneDef& a_def, const std::vector<RE::Actor*>& a_actors,
		const std::vector<std::string>& a_roleNames, std::vector<RE::Actor*>& a_ordered, std::string& a_error)
	{
		if (a_actors.size() != a_def.roles.size()) {
			a_error = std::format("Scene needs {} cast member(s), but {} selected",
				a_def.roles.size(), a_actors.size());
			return false;
		}
		if (!a_roleNames.empty() && a_roleNames.size() != a_actors.size()) {
			a_error = std::format("Expected {} role name(s), but {} supplied",
				a_actors.size(), a_roleNames.size());
			return false;
		}

		a_ordered = a_actors;
		if (!a_roleNames.empty()) {
			a_ordered.assign(a_def.roles.size(), nullptr);
			for (std::size_t i = 0; i < a_actors.size(); ++i) {
				const auto want = ToLower(a_roleNames[i]);
				const auto role = std::ranges::find_if(a_def.roles,
					[&](const Registry::SceneRole& a_candidate) { return ToLower(a_candidate.name) == want; });
				if (role == a_def.roles.end()) {
					a_error = "Unknown role '" + a_roleNames[i] + "'";
					return false;
				}
				const auto index = static_cast<std::size_t>(std::distance(a_def.roles.begin(), role));
				if (a_ordered[index]) {
					a_error = "Role '" + a_roleNames[i] + "' was assigned twice";
					return false;
				}
				a_ordered[index] = a_actors[i];
			}
		}

		std::unordered_set<RE::Actor*> unique;
		for (std::size_t i = 0; i < a_ordered.size(); ++i) {
			if (!a_ordered[i] || !unique.insert(a_ordered[i]).second) {
				a_error = "Cast contains a missing or duplicate actor";
				return false;
			}
			if (!RoleAccepts(a_def.roles[i], a_ordered[i])) {
				a_error = "A selected actor does not satisfy role '" + a_def.roles[i].name + "'";
				return false;
			}
		}
		return true;
	}

	bool AnchorAccepts(const Registry::SceneDef& a_def, RE::TESObjectREFR* a_ref, RE::BGSKeyword** a_matchedKeyword)
	{
		if (a_matchedKeyword) {
			*a_matchedKeyword = nullptr;
		}
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
		for (const auto kwId : a_def.anchorKeywords) {
			auto* kw = RE::TESForm::LookupByID<RE::BGSKeyword>(kwId);
			if (kw && a_ref->HasKeyword(kw)) {
				if (a_matchedKeyword) {
					*a_matchedKeyword = kw;
				}
				return true;
			}
		}
		return false;
	}

	AnchorMatchCache::AnchorMatchCache(RE::TESObjectREFR* a_ref) :
		ref(a_ref)
	{
		if (a_ref) {
			if (const auto base = a_ref->GetBaseObject()) {
				baseId = base->GetFormID();
			}
		}
	}

	bool AnchorMatchCache::Accepts(const Registry::SceneDef& a_def)
	{
		if (!ref) {
			return false;
		}
		if (baseId != 0) {
			for (const auto b : a_def.anchorBaseForms) {
				if (b == baseId) {
					return true;
				}
			}
		}
		for (const auto kwId : a_def.anchorKeywords) {
			auto it = kwHits.find(kwId);
			if (it == kwHits.end()) {
				auto* kw = RE::TESForm::LookupByID<RE::BGSKeyword>(kwId);
				it = kwHits.emplace(kwId, kw && ref->HasKeyword(kw)).first;
			}
			if (it->second) {
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
			return {};  // gender-agnostic: the {gender} tag drops out
		}
	}

	std::optional<Candidate> Pick(const std::vector<RE::Actor*>& a_actors, const TagQuery& a_query,
		RE::TESObjectREFR* a_anchor, AnchorMode a_mode)
	{
		const auto actorCount = static_cast<std::int32_t>(a_actors.size());
		REX::DEBUG("[Match] searching for {} actor(s), tags {}, {}",
			actorCount, DescribeQuery(a_query), DescribeAnchorSearch(a_anchor, a_mode));
		if (a_actors.empty()) {
			REX::DEBUG("[Match] no match (no actors supplied)");
			return std::nullopt;
		}
		auto pool = BuildPool(a_query, a_actors, a_anchor, a_mode);
		if (pool.empty()) {
			REX::DEBUG("[Match] no match (no scene def fit {} role(s) + tags + anchor filters + a complete actor binding; {})",
				actorCount, DescribeAnchorSearch(a_anchor, a_mode));
			return std::nullopt;
		}
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
		// uint64 sum is overflow-safe for the [1,1e6] weight cap.
		static thread_local std::mt19937 rng{ std::random_device{}() };
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
		return *tier.back();  // numerical guard
	}
}
