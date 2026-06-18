// Tests for PackRegistry: JSON pack scan, schema gating, duplicate-id handling,
// and BuildScenePlan resolution (files + placements + timer/loops). LoadAll reads
// the fixtures under test/fixtures/Data/OSF (main() chdir's there first).
#include "framework/TestHarness.h"

#include "Registry/PackRegistry.h"
#include "Util/Math.h"

using OSF::Registry::PackRegistry;
using OSF::Registry::AnimationDef;
using OSF::Registry::SlotGender;

namespace
{
	const AnimationDef* Find(std::string_view a_id)
	{
		static const AnimationDef* found = nullptr;
		found = nullptr;
		PackRegistry::GetSingleton().ForEachAnim([&](const AnimationDef& a_def) {
			if (a_def.id == a_id) {
				found = &a_def;
			}
		});
		return found;
	}
}

OSF_TEST_CASE(PackRegistry_loads_valid_anims)
{
	auto& reg = PackRegistry::GetSingleton();
	// solo, pair, shadowed are the three valid anims in pack_basic.json.
	CHECK(reg.Size() >= 3);
	CHECK(Find("osf.test.solo") != nullptr);
	CHECK(Find("osf.test.pair") != nullptr);
	CHECK(Find("osf.test.shadowed") != nullptr);
}

OSF_TEST_CASE(PackRegistry_parses_actors_and_genders)
{
	const auto* pair = Find("osf.test.pair");
	CHECK(pair != nullptr);
	if (pair) {
		CHECK_EQ(pair->actors.size(), static_cast<size_t>(2));
		CHECK(pair->actors[0].gender == SlotGender::kMale);
		CHECK(pair->actors[1].gender == SlotGender::kFemale);
		CHECK_EQ(pair->stages.size(), static_cast<size_t>(2));
	}
}

OSF_TEST_CASE(PackRegistry_rejects_bad_schema_and_dupes)
{
	// pack_badschema.json (schema 99) contributes nothing; its anim id is absent.
	CHECK(Find("osf.badschema.anim") == nullptr);
	// pack_dupe.json re-declares osf.test.solo (different case); first-load wins, so
	// exactly one solo entry survives and it keeps the original pack's name.
	const auto* solo = Find("osf.test.solo");
	CHECK(solo != nullptr);
	if (solo) {
		CHECK_EQ(solo->pack, std::string("OSFTestPack"));
	}
}

OSF_TEST_CASE(PackRegistry_build_scene_plan_resolves_files)
{
	auto plan = PackRegistry::GetSingleton().BuildScenePlan("osf.test.pair", 2);
	CHECK(plan.has_value());
	if (plan) {
		CHECK_EQ(plan->animId, std::string("osf.test.pair"));
		CHECK_EQ(plan->stages.size(), static_cast<size_t>(2));
		// Stage 0: two clips, one per actor, in actor order.
		CHECK_EQ(plan->stages[0].files.size(), static_cast<size_t>(2));
		CHECK_EQ(plan->stages[0].files[0], std::string("OSF\\Anim\\pairA0.glb"));
		CHECK_EQ(plan->stages[0].files[1], std::string("OSF\\Anim\\pairB0.glb"));
		CHECK_EQ(plan->stages[0].placements.size(), static_cast<size_t>(2));
		// Stage timing carried through.
		CHECK_NEAR(plan->stages[0].timer, 3.0f, 1e-4f);
		CHECK_EQ(plan->stages[1].loops, 2);
	}
}

OSF_TEST_CASE(PackRegistry_build_scene_plan_applies_offsets)
{
	auto plan = PackRegistry::GetSingleton().BuildScenePlan("osf.test.pair", 2);
	CHECK(plan.has_value());
	if (plan) {
		// Actor 1 has a default offset of y=1.0, heading=180deg in the fixture; with
		// no per-clip override on stage 0 the default placement carries through.
		const auto& p = plan->stages[0].placements[1];
		CHECK_NEAR(p.y, 1.0f, 1e-4f);
		CHECK_NEAR(p.heading, static_cast<float>(180.0 * OSF::Util::kDegToRad), 1e-3f);
	}
}

OSF_TEST_CASE(PackRegistry_build_scene_plan_rejects_wrong_arity)
{
	// pair needs two actors; asking for one (or for an unknown id) yields nullopt.
	CHECK(!PackRegistry::GetSingleton().BuildScenePlan("osf.test.pair", 1).has_value());
	CHECK(!PackRegistry::GetSingleton().BuildScenePlan("does.not.exist", 2).has_value());
}

OSF_TEST_CASE(PackRegistry_lookup_is_case_insensitive)
{
	CHECK(PackRegistry::GetSingleton().BuildScenePlan("OSF.TEST.SOLO", 1).has_value());
}
