#include "Matchmaking/Matchmaker.h"
#include "Profiling/Profile.h"
#include "Matchmaking/RoleBinding.h"

#include "Registry/ContentRegistry.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <format>
#include <random>
#include <unordered_set>
#include <spdlog/spdlog.h>

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

		// What role filters read from one actor, resolved once per query instead of per role x permutation.
		struct ActorFacts
		{
			explicit ActorFacts(RE::Actor* a_actor) :
				actor(a_actor), sex(SexOf(a_actor)), race(a_actor ? a_actor->race : nullptr)
			{}

			bool HasKeyword(RE::TESFormID a_keyword)
			{
				auto [it, fresh] = keywordHits.try_emplace(a_keyword, false);
				if (fresh) {
					// The registry stores FormIDs, not pointers.
					it->second = ActorHasKeyword(actor, RE::TESForm::LookupByID<RE::BGSKeyword>(a_keyword));
				}
				return it->second;
			}

			RE::Actor*                                actor;
			RE::SEX                                   sex;
			RE::TESRace*                              race;
			std::unordered_map<RE::TESFormID, bool>   keywordHits;
		};

		bool RoleAcceptsFacts(const Registry::SceneRole& a_role, ActorFacts& a_facts)
		{
			if (!a_facts.actor) {
				return false;
			}
			if (a_role.gender != Registry::RoleGender::kAny && !RoleGenderAccepts(a_role.gender, a_facts.sex)) {
				return false;
			}
			if (!a_role.keywords.empty() &&
				std::ranges::none_of(a_role.keywords, [&](const auto a_kw) { return a_facts.HasKeyword(a_kw); })) {
				return false;
			}
			if (!a_role.races.empty() &&
				(!a_facts.race || std::ranges::find(a_role.races, a_facts.race->GetFormID()) == a_role.races.end())) {
				return false;
			}
			return true;
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

		// a_query is lowercased; a_have is the def's precomputed lowercase NodeDef::tagSet (hot path).
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

		// Pick guarantees a_actors is non-empty.
		std::vector<Candidate> BuildPool(const TagQuery& a_query, const std::vector<RE::Actor*>& a_actors,
			RE::TESObjectREFR* a_anchor, AnchorMode a_mode, std::string_view a_exclude,
			std::optional<Animation::WorldPlacementMode> a_placement, const RoleConstraints& a_roles)
		{
			OSF_PROFILE_SCOPE("Match.BuildPool");
			const auto count = static_cast<std::int32_t>(a_actors.size());
			const TagQuery q{ Lower(a_query.allOf), Lower(a_query.anyOf), Lower(a_query.noneOf) };

			// Vanilla packs share a handful of AnimFurn* keywords across thousands of defs.
			AnchorMatchCache anchorCache(a_anchor);

			std::vector<ActorFacts> facts;
			facts.reserve(a_actors.size());
			for (auto* actor : a_actors) {
				facts.emplace_back(actor);
			}
			// compatible[role * count + actor]: evaluated once per def so permutations are table lookups.
			const auto n = static_cast<std::size_t>(count);
			std::vector<std::uint8_t> compatible;

			std::vector<Candidate> pool;

			Registry::ContentRegistry::GetSingleton().ForEachDef([&](const Registry::NodeDef& a_def) {
				if (a_def.id == a_exclude || (a_placement && a_def.worldPlacement != *a_placement)) return;
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
				compatible.assign(n * n, 0);
				for (std::size_t role = 0; role < n; ++role) {
					for (std::size_t actorIndex = 0; actorIndex < n; ++actorIndex) {
						compatible[role * n + actorIndex] =
							(a_roles.empty() || RoleNameMatches(a_roles[actorIndex], a_def.roles[role].name)) &&
							RoleAcceptsFacts(a_def.roles[role], facts[actorIndex]);
					}
				}
				auto order = FindRoleOrder(n,
					[&](std::size_t a_role, std::size_t a_actorIndex) { return compatible[a_role * n + a_actorIndex] != 0; });
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

		std::string DescribeRoles(const RoleConstraints& a_roles)
		{
			std::string out;
			for (std::size_t i = 0; i < a_roles.size(); i++) {
				if (i) {
					out += ',';
				}
				out += a_roles[i].empty() ? "*" : a_roles[i];
			}
			return out;
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
		ActorFacts facts{ a_actor };
		return RoleAcceptsFacts(a_role, facts);
	}

	bool BindSceneRoles(const Registry::NodeDef& a_def, const std::vector<RE::Actor*>& a_actors,
		const std::vector<std::string>& a_roleNames, std::vector<RE::Actor*>& a_ordered, std::string& a_error)
	{
		std::vector<std::string> names;
		for (const auto& role : a_def.roles) names.push_back(role.name);
		return BindRoleNames(names, a_actors, a_roleNames, a_ordered, a_error,
			[&](std::size_t role, RE::Actor* actor) { return RoleAccepts(a_def.roles[role], actor); });
	}

	bool AnchorAccepts(const Registry::NodeDef& a_def, RE::TESObjectREFR* a_ref, RE::BGSKeyword** a_matchedKeyword)
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

	bool AnchorMatchCache::Accepts(const Registry::NodeDef& a_def)
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

	static std::optional<Candidate> PickImpl(const std::vector<RE::Actor*>& a_actors, const TagQuery& a_query,
		RE::TESObjectREFR* a_anchor, AnchorMode a_mode, std::string_view a_exclude,
		std::optional<Animation::WorldPlacementMode> a_placement, const RoleConstraints& a_roles)
	{
		OSF_PROFILE_SCOPE("Match.Pick");
		const auto actorCount = static_cast<std::int32_t>(a_actors.size());
		const bool debugLog = spdlog::should_log(spdlog::level::debug);
		if (debugLog) {
			REX::DEBUG("[Match] searching for {} actor(s), tags {}, roles [{}], {}",
				actorCount, DescribeQuery(a_query), DescribeRoles(a_roles), DescribeAnchorSearch(a_anchor, a_mode));
		}
		if (a_actors.empty()) {
			REX::DEBUG("[Match] no match (no actors supplied)");
			return std::nullopt;
		}
		if (!RoleConstraintsFit(a_roles, a_actors.size())) {
			REX::DEBUG("[Match] no match ({} role constraint(s) for {} actor(s))", a_roles.size(), actorCount);
			return std::nullopt;
		}
		auto pool = BuildPool(a_query, a_actors, a_anchor, a_mode, a_exclude, a_placement, a_roles);
		if (pool.empty()) {
			if (debugLog) {
				REX::DEBUG("[Match] no match (no scene def fit {} role(s) + tags + anchor filters + a complete actor binding; {})",
					actorCount, DescribeAnchorSearch(a_anchor, a_mode));
			}
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

	std::optional<Candidate> Pick(const std::vector<RE::Actor*>& actors, const TagQuery& query,
		RE::TESObjectREFR* anchor, AnchorMode mode, const RoleConstraints& roles)
	{
		return PickImpl(actors, query, anchor, mode, {}, std::nullopt, roles);
	}

	std::optional<Candidate> PickReplacement(const std::vector<RE::Actor*>& actors, const TagQuery& query,
		RE::TESObjectREFR* anchor, std::string_view exclude, Animation::WorldPlacementMode placement,
		const RoleConstraints& roles, AnchorMode mode)
	{
		return PickImpl(actors, query, anchor, mode, exclude, placement, roles);
	}
}
