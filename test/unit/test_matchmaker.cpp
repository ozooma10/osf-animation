// Tests for the unified matchmaker over both registries: tag queries, the
// scene-def-shadows-pack rule, deterministic discovery ordering, role-filter
// acceptance, and weighted/priority Pick. Uses real stub actors (we own RE::Actor
// here) to drive the full filter-aware binding path.
#include "framework/TestHarness.h"

#include "Matchmaking/Matchmaker.h"
#include "Registry/SceneRegistry.h"

using OSF::Matchmaking::FindIds;
using OSF::Matchmaking::Pick;
using OSF::Matchmaking::RoleAccepts;
using OSF::Matchmaking::TagQuery;
using OSF::Registry::SceneRole;
using OSF::Registry::SlotGender;

namespace
{
	TagQuery AllOf(std::vector<std::string> a_tags)
	{
		return TagQuery{ std::move(a_tags), {}, {} };
	}

	bool Contains(const std::vector<std::string>& a_ids, std::string_view a_id)
	{
		for (const auto& s : a_ids) {
			if (s == a_id) {
				return true;
			}
		}
		return false;
	}

	const std::vector<RE::Actor*> kNoActors{};
}

OSF_TEST_CASE(Matchmaker_discovery_orders_priority_then_id)
{
	// Two-actor "pair" candidates: scene hi (p10), scene basic (p0), pack pair (p0).
	auto ids = FindIds(2, AllOf({ "pair" }), kNoActors);
	CHECK_EQ(ids.size(), static_cast<size_t>(3));
	if (ids.size() == 3) {
		CHECK_EQ(ids[0], std::string("osf.scene.hi"));     // highest priority first
		CHECK_EQ(ids[1], std::string("osf.scene.basic"));  // p0 tie -> id ascending
		CHECK_EQ(ids[2], std::string("osf.test.pair"));
	}
}

OSF_TEST_CASE(Matchmaker_scene_def_shadows_same_id_pack)
{
	// Both a scene def and a pack declare osf.test.shadowed; the pack is suppressed.
	auto ids = FindIds(2, AllOf({ "shadowtag" }), kNoActors);
	CHECK_EQ(ids.size(), static_cast<size_t>(1));
	CHECK(Contains(ids, "osf.test.shadowed"));
}

OSF_TEST_CASE(Matchmaker_actor_count_filter)
{
	// solo (1-actor pack) appears for count 1, not for count 2.
	CHECK(Contains(FindIds(1, AllOf({ "solo" }), kNoActors), "osf.test.solo"));
	CHECK(FindIds(1, AllOf({ "pair" }), kNoActors).empty());      // pair things need 2
	CHECK(!Contains(FindIds(2, AllOf({ "solo" }), kNoActors), "osf.test.solo"));
}

OSF_TEST_CASE(Matchmaker_anyof_and_noneof)
{
	// anyOf scenetag -> only the two scene defs carry it (the pack does not).
	auto anyIds = FindIds(2, TagQuery{ {}, { "scenetag" }, {} }, kNoActors);
	CHECK_EQ(anyIds.size(), static_cast<size_t>(2));
	CHECK(Contains(anyIds, "osf.scene.basic"));
	CHECK(Contains(anyIds, "osf.scene.hi"));
	// noneOf test -> excludes the pack (tagged "test"), keeps the scene defs.
	auto noneIds = FindIds(2, TagQuery{ { "pair" }, {}, { "test" } }, kNoActors);
	CHECK_EQ(noneIds.size(), static_cast<size_t>(2));
	CHECK(!Contains(noneIds, "osf.test.pair"));
}

OSF_TEST_CASE(Matchmaker_role_accepts_gender)
{
	RE::TESNPC maleNpc;
	maleNpc.sex = RE::SEX::kMale;
	RE::Actor male;
	male.npc = &maleNpc;

	RE::TESNPC femaleNpc;
	femaleNpc.sex = RE::SEX::kFemale;
	RE::Actor female;
	female.npc = &femaleNpc;

	SceneRole maleRole;
	maleRole.gender = SlotGender::kMale;
	CHECK(RoleAccepts(maleRole, &male));
	CHECK(!RoleAccepts(maleRole, &female));
	CHECK(!RoleAccepts(maleRole, nullptr));  // a null actor never accepts

	SceneRole anyRole;
	anyRole.gender = SlotGender::kAny;
	CHECK(RoleAccepts(anyRole, &male));
	CHECK(RoleAccepts(anyRole, &female));
}

OSF_TEST_CASE(Matchmaker_role_accepts_keyword_on_npc_or_race)
{
	RE::BGSKeyword wantKw;
	RE::BGSKeyword otherKw;

	RE::TESNPC npc;          // carries the keyword on the actorbase
	npc.keywords.push_back(&wantKw);
	RE::Actor viaNpc;
	viaNpc.npc = &npc;

	RE::TESNPC plainNpc;
	RE::TESRace race;        // carries it on the race instead
	race.keywords.push_back(&wantKw);
	RE::Actor viaRace;
	viaRace.npc = &plainNpc;
	viaRace.race = &race;

	RE::TESNPC noneNpc;
	RE::Actor without;
	without.npc = &noneNpc;

	SceneRole role;
	role.gender = SlotGender::kAny;
	role.keywords.push_back(&wantKw);

	CHECK(RoleAccepts(role, &viaNpc));    // keyword on actorbase
	CHECK(RoleAccepts(role, &viaRace));   // keyword on race
	CHECK(!RoleAccepts(role, &without));  // neither carries it

	SceneRole otherRole;
	otherRole.gender = SlotGender::kAny;
	otherRole.keywords.push_back(&otherKw);
	CHECK(!RoleAccepts(otherRole, &viaNpc));
}

OSF_TEST_CASE(Matchmaker_role_accepts_race)
{
	RE::TESRace raceA;
	RE::TESRace raceB;
	RE::TESNPC npc;

	RE::Actor actorA;
	actorA.npc = &npc;
	actorA.race = &raceA;

	SceneRole role;
	role.gender = SlotGender::kAny;
	role.races.push_back(&raceA);
	CHECK(RoleAccepts(role, &actorA));

	role.races.clear();
	role.races.push_back(&raceB);
	CHECK(!RoleAccepts(role, &actorA));
}

OSF_TEST_CASE(Matchmaker_pick_takes_top_priority_tier)
{
	RE::TESNPC maleNpc;
	maleNpc.sex = RE::SEX::kMale;
	RE::TESNPC femaleNpc;
	femaleNpc.sex = RE::SEX::kFemale;
	RE::Actor male;
	male.npc = &maleNpc;
	RE::Actor female;
	female.npc = &femaleNpc;

	std::vector<RE::Actor*> actors{ &male, &female };
	// hi (p10), basic (p0), pack pair (p0) all bind to (M, F); the top tier is hi
	// alone, so the weighted pick is deterministic.
	auto pick = Pick(actors, AllOf({ "pair" }));
	CHECK(pick.has_value());
	if (pick) {
		CHECK_EQ(pick->id, std::string("osf.scene.hi"));
		CHECK_EQ(pick->order.size(), static_cast<size_t>(2));
	}
}

OSF_TEST_CASE(Matchmaker_pick_returns_nullopt_when_no_binding)
{
	RE::TESNPC maleNpc;
	maleNpc.sex = RE::SEX::kMale;
	RE::Actor m1;
	m1.npc = &maleNpc;
	RE::Actor m2;
	m2.npc = &maleNpc;

	// Two males cannot satisfy any male/female "pair" candidate.
	std::vector<RE::Actor*> twoMales{ &m1, &m2 };
	CHECK(!Pick(twoMales, AllOf({ "pair" })).has_value());

	// The empty-actor guard also yields nullopt.
	CHECK(!Pick(kNoActors, AllOf({ "pair" })).has_value());
}
