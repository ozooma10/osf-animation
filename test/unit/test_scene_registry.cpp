// Tests for SceneRegistry: scene-graph JSON parse, validation diagnostics, track
// lanes, and the SceneDef helpers. Form-ref filters fail-soft offline (no data
// handler) and reject their scene -- exercised as a negative case.
#include "framework/TestHarness.h"

#include "Registry/SceneRegistry.h"

using OSF::Registry::SceneRegistry;
using OSF::Registry::SceneDef;
using OSF::Registry::SlotGender;
using OSF::Registry::LoopMode;
using OSF::Registry::CuePos;
using OSF::Registry::ActionPos;

namespace
{
	bool AnyErrorContains(std::string_view a_needle)
	{
		for (const auto& e : SceneRegistry::GetSingleton().LoadErrors()) {
			if (e.find(a_needle) != std::string::npos) {
				return true;
			}
		}
		return false;
	}
}

OSF_TEST_CASE(SceneRegistry_loads_valid_scenes)
{
	auto& reg = SceneRegistry::GetSingleton();
	CHECK(reg.Find("osf.scene.basic") != nullptr);
	CHECK(reg.Find("osf.scene.hi") != nullptr);
	CHECK(reg.Find("osf.test.shadowed") != nullptr);
	// Case-insensitive lookup.
	CHECK(reg.Find("OSF.SCENE.BASIC") != nullptr);
}

OSF_TEST_CASE(SceneRegistry_parses_graph_structure)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.scene.basic");
	CHECK(def != nullptr);
	if (def) {
		CHECK_EQ(def->priority, 0);
		CHECK_EQ(def->roles.size(), static_cast<size_t>(2));
		CHECK(def->roles[0].gender == SlotGender::kMale);
		CHECK(def->roles[1].gender == SlotGender::kFemale);
		CHECK_EQ(def->entry, std::string("intro"));
		CHECK_EQ(def->nodes.size(), static_cast<size_t>(2));
		const auto* intro = def->FindNode("intro");
		CHECK(intro != nullptr);
		if (intro) {
			CHECK(intro->loopMode == LoopMode::kOnce);
			CHECK_EQ(intro->edges.size(), static_cast<size_t>(1));
			CHECK_EQ(intro->edges[0].to, std::string("loop"));
		}
	}
}

OSF_TEST_CASE(SceneRegistry_parses_all_track_lanes)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.scene.basic");
	CHECK(def != nullptr);
	if (!def) {
		return;
	}
	const auto* intro = def->FindNode("intro");
	CHECK(intro != nullptr);
	if (!intro) {
		return;
	}
	// cue track: one numeric cue "beat" at 0.25.
	CHECK_EQ(intro->cues.size(), static_cast<size_t>(1));
	CHECK_EQ(intro->cues[0].id, std::string("beat"));
	CHECK(intro->cues[0].pos == CuePos::kFraction);
	CHECK_NEAR(intro->cues[0].fraction, 0.25f, 1e-4f);
	// action track: a built-in fade + a role-targeted control lock.
	CHECK_EQ(intro->actions.size(), static_cast<size_t>(2));
	CHECK(intro->actions[0].pos == ActionPos::kEnter);
	// sound + camera lanes each parsed one entry.
	CHECK_EQ(intro->sounds.size(), static_cast<size_t>(1));
	CHECK_EQ(intro->cameras.size(), static_cast<size_t>(1));
	CHECK_EQ(intro->cameras[0].state, std::string("thirdperson_hold"));
}

OSF_TEST_CASE(SceneRegistry_parses_camera_state_overrides)
{
	// camera_states.scene.json: an enter-anchored vanity_orbit on one node and freefly on the next.
	const auto* def = SceneRegistry::GetSingleton().Find("osf.scene.camerastates");
	CHECK(def != nullptr);
	if (!def) {
		return;
	}
	const auto* orbit = def->FindNode("orbit");
	CHECK(orbit != nullptr);
	if (orbit) {
		CHECK_EQ(orbit->cameras.size(), static_cast<size_t>(1));
		CHECK_EQ(orbit->cameras[0].state, std::string("vanity_orbit"));
	}
	const auto* fly = def->FindNode("fly");
	CHECK(fly != nullptr);
	if (fly) {
		CHECK_EQ(fly->cameras.size(), static_cast<size_t>(1));
		CHECK_EQ(fly->cameras[0].state, std::string("freefly"));
	}
}

OSF_TEST_CASE(SceneRegistry_rejects_unknown_camera_state)
{
	// bad_camera.scene.json uses an unsupported camera state -> rejected fail-soft + recorded.
	CHECK(SceneRegistry::GetSingleton().Find("osf.bad.camera") == nullptr);
	CHECK(AnyErrorContains("unknown camera state"));
}

OSF_TEST_CASE(SceneRegistry_player_lock_defaults_on_and_opts_out)
{
	auto& reg = SceneRegistry::GetSingleton();
	// Default: a scene with no 'lockPlayer' key disables player input when the player is in.
	const auto* on = reg.Find("osf.scene.basic");
	CHECK(on != nullptr);
	if (on) {
		CHECK(on->lockPlayer);
	}
	// Opt-out: "lockPlayer": false parses through and leaves the player free.
	const auto* off = reg.Find("osf.scene.lockplayeroff");
	CHECK(off != nullptr);
	if (off) {
		CHECK(!off->lockPlayer);
	}
}

OSF_TEST_CASE(SceneRegistry_strip_actors_defaults_on_and_opts_out)
{
	auto& reg = SceneRegistry::GetSingleton();
	// Default: a scene with no 'stripActors' key strips every participant's apparel.
	const auto* on = reg.Find("osf.scene.basic");
	CHECK(on != nullptr);
	if (on) {
		CHECK(on->stripActors);
	}
	// Opt-out: "stripActors": false parses through and keeps actors clothed.
	const auto* off = reg.Find("osf.scene.stripactorsoff");
	CHECK(off != nullptr);
	if (off) {
		CHECK(!off->stripActors);
	}
}

OSF_TEST_CASE(SceneRegistry_linear_stage_index)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.scene.basic");
	CHECK(def != nullptr);
	if (def) {
		CHECK_EQ(def->LinearStageOf("intro"), 0);
		CHECK_EQ(def->LinearStageOf("loop"), 1);
		CHECK_EQ(def->LinearStageOf("nope"), -1);
	}
}

OSF_TEST_CASE(SceneRegistry_records_bad_entry_error)
{
	// bad_entry.scene.json names an entry that is not a node -> rejected + recorded.
	CHECK(SceneRegistry::GetSingleton().Find("osf.bad.entry") == nullptr);
	CHECK(AnyErrorContains("entry"));
}

OSF_TEST_CASE(SceneRegistry_rejects_unresolved_formref)
{
	// formref.scene.json has a keyword form-ref; with no data handler offline it
	// cannot resolve, so the scene is rejected fail-soft and recorded.
	CHECK(SceneRegistry::GetSingleton().Find("osf.formref.scene") == nullptr);
	CHECK(AnyErrorContains("formref"));
}

OSF_TEST_CASE(SceneRegistry_reports_duplicate_scene_id)
{
	// dup_b.scene.json re-declares the id from dup_a.scene.json; first wins, the
	// second is logged as a duplicate. The id still resolves (to the first).
	CHECK(SceneRegistry::GetSingleton().Find("osf.dup.scene") != nullptr);
	CHECK(AnyErrorContains("duplicate scene id"));
}
