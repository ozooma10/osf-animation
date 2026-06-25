// Tests for the unified *.osf.json scene schema (RFC-unified-animation-schema.md, Phase 1):
// the clip/stages/nodes parser, the linear->node-chain desugar, `at`-lowered track lanes, the
// unified roles[] list, and the load-time validation (use XOR stages, reserved ids, dangling
// use, duplicate ids). Fixtures: test/fixtures/Data/OSF/u_*.osf.json (main() chdir's there).
#include "framework/TestHarness.h"

#include "Registry/SceneRegistry.h"
#include "Util/Math.h"

using OSF::Registry::SceneRegistry;
using OSF::Registry::SceneDef;
using OSF::Registry::SlotGender;
using OSF::Registry::LoopMode;
using OSF::Registry::EdgeWhen;
using OSF::Registry::CuePos;
using OSF::Registry::ActionPos;
using OSF::Registry::SoundPos;
using OSF::Registry::CameraPos;

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

OSF_TEST_CASE(Osf_minimal_clip_desugars_to_single_play_once_node)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.u.minimal");
	CHECK(def != nullptr);
	if (!def) {
		return;
	}
	// `clip` -> one inferred role, one play-once stage, one desugared node ending at $end.
	CHECK_EQ(def->roles.size(), static_cast<size_t>(1));
	CHECK_EQ(def->entry, std::string("#s0"));
	CHECK_EQ(def->linearStages.size(), static_cast<size_t>(1));
	CHECK_EQ(def->nodes.size(), static_cast<size_t>(1));
	const auto& n = def->nodes[0];
	CHECK_EQ(n.id, std::string("#s0"));
	CHECK(n.loopMode == LoopMode::kCount);
	CHECK_EQ(n.loopCount, 1);
	CHECK_EQ(n.stages.size(), static_cast<size_t>(1));
	CHECK_EQ(n.stages[0].clips.size(), static_cast<size_t>(1));
	CHECK_EQ(n.stages[0].clips[0].file, std::string("OSF/Anim/wave.glb"));
	// loops edge (auto-end after one play) + a default advance edge (manual step), both to $end.
	CHECK_EQ(n.edges.size(), static_cast<size_t>(2));
	CHECK_EQ(n.edges[0].to, std::string("$end"));
	CHECK(n.edges[0].when == EdgeWhen::kLoops);
	CHECK_EQ(n.edges[1].to, std::string("$end"));
	CHECK(n.edges[1].when == EdgeWhen::kAdvance);
	CHECK(n.edges[1].isDefault);
}

OSF_TEST_CASE(Osf_idle_stage_holds_forever)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.u.idle");
	CHECK(def != nullptr);
	if (!def) {
		return;
	}
	// loops:0 + timer:0 -> hold-forever node with no auto-end edge, but still a default advance
	// edge so the player can step off it manually.
	CHECK_EQ(def->nodes.size(), static_cast<size_t>(1));
	const auto& n = def->nodes[0];
	CHECK(n.loopMode == LoopMode::kHold);
	CHECK(n.loopForever);
	CHECK_EQ(n.edges.size(), static_cast<size_t>(1));
	CHECK(n.edges[0].when == EdgeWhen::kAdvance);
	CHECK(n.edges[0].isDefault);
	CHECK_EQ(n.edges[0].to, std::string("$end"));
}

OSF_TEST_CASE(Osf_multistage_desugars_to_node_chain)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.u.multistage");
	CHECK(def != nullptr);
	if (!def) {
		return;
	}
	CHECK_EQ(def->nodes.size(), static_cast<size_t>(3));
	CHECK_EQ(def->entry, std::string("#s0"));
	CHECK_EQ(def->linearStages.size(), static_cast<size_t>(3));
	CHECK_EQ(def->LinearStageOf("#s1"), 1);

	// Stage 0: untimed -> play-once (count 1) -> loops edge to #s1, plus a default advance edge to #s1.
	const auto& s0 = def->nodes[0];
	CHECK_EQ(s0.id, std::string("#s0"));
	CHECK(s0.loopMode == LoopMode::kCount);
	CHECK_EQ(s0.loopCount, 1);
	CHECK_EQ(s0.edges.size(), static_cast<size_t>(2));
	CHECK_EQ(s0.edges[0].to, std::string("#s1"));
	CHECK(s0.edges[0].when == EdgeWhen::kLoops);
	CHECK(s0.edges[1].when == EdgeWhen::kAdvance);
	CHECK(s0.edges[1].isDefault);
	CHECK_EQ(s0.edges[1].to, std::string("#s1"));

	// Stage 1: timer -> hold + timer edge to #s2, plus a default advance edge to #s2.
	const auto& s1 = def->nodes[1];
	CHECK(s1.loopMode == LoopMode::kHold);
	CHECK_NEAR(s1.timerSec, 6.0f, 1e-4f);
	CHECK_EQ(s1.edges.size(), static_cast<size_t>(2));
	CHECK_EQ(s1.edges[0].to, std::string("#s2"));
	CHECK(s1.edges[0].when == EdgeWhen::kTimer);
	CHECK(s1.edges[1].when == EdgeWhen::kAdvance);
	CHECK_EQ(s1.edges[1].to, std::string("#s2"));

	// Stage 2 (last): loop-count -> count(2) + loops edge to $end, plus a default advance edge to $end.
	const auto& s2 = def->nodes[2];
	CHECK(s2.loopMode == LoopMode::kCount);
	CHECK_EQ(s2.loopCount, 2);
	CHECK_EQ(s2.edges.size(), static_cast<size_t>(2));
	CHECK_EQ(s2.edges[0].to, std::string("$end"));
	CHECK(s2.edges[0].when == EdgeWhen::kLoops);
	CHECK(s2.edges[1].when == EdgeWhen::kAdvance);
	CHECK_EQ(s2.edges[1].to, std::string("$end"));

	// Unified roles[]: anonymous slot 0, slot 1 carries a default offset.
	CHECK_EQ(def->roles.size(), static_cast<size_t>(2));
	CHECK(def->roles[0].name.empty());
	CHECK_NEAR(def->roles[1].offset.y, 1.0f, 1e-4f);
	CHECK_NEAR(def->roles[1].offset.heading, static_cast<float>(180.0 * OSF::Util::kDegToRad), 1e-3f);
	CHECK_EQ(def->nodes[0].stages[0].clips.size(), static_cast<size_t>(2));
	CHECK_EQ(def->nodes[0].stages[0].clips[0].file, std::string("OSF/Anim/a0.glb"));
}

OSF_TEST_CASE(Osf_graph_use_and_inline_nodes)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.u.graph");
	CHECK(def != nullptr);
	if (!def) {
		return;
	}
	CHECK_EQ(def->entry, std::string("approach"));
	CHECK_EQ(def->nodes.size(), static_cast<size_t>(2));

	const auto* approach = def->FindNode("approach");
	CHECK(approach != nullptr);
	if (approach) {
		// `use` node: references another scene by id, no inline stages.
		CHECK_EQ(approach->use, std::string("osf.u.shared"));
		CHECK(approach->stages.empty());
		CHECK(approach->loopMode == LoopMode::kOnce);
		CHECK_EQ(approach->edges.size(), static_cast<size_t>(1));
		CHECK_EQ(approach->edges[0].to, std::string("main"));
		CHECK(approach->edges[0].when == EdgeWhen::kEnd);
	}

	const auto* main = def->FindNode("main");
	CHECK(main != nullptr);
	if (main) {
		// inline-stages node: carries its own clips, no `use`.
		CHECK(main->use.empty());
		CHECK_EQ(main->stages.size(), static_cast<size_t>(1));
		CHECK_EQ(main->stages[0].clips.size(), static_cast<size_t>(2));
		CHECK(main->loopMode == LoopMode::kHold);
		// branch (finish -> $end, default) + self-loop (tease -> main).
		CHECK_EQ(main->edges.size(), static_cast<size_t>(2));
		CHECK_EQ(main->edges[0].id, std::string("finish"));
		CHECK(main->edges[0].isDefault);
		CHECK_EQ(main->edges[0].to, std::string("$end"));
		CHECK(main->edges[0].when == EdgeWhen::kAdvance);
		CHECK_EQ(main->edges[1].id, std::string("tease"));
		CHECK_EQ(main->edges[1].to, std::string("main"));
	}
}

OSF_TEST_CASE(Osf_graph_track_lanes_and_at_lowering)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.u.graph");
	CHECK(def != nullptr);
	if (!def) {
		return;
	}
	const auto* main = def->FindNode("main");
	CHECK(main != nullptr);
	if (!main) {
		return;
	}
	// cue: numeric `at` -> kFraction(0.5).
	CHECK_EQ(main->cues.size(), static_cast<size_t>(1));
	CHECK_EQ(main->cues[0].id, std::string("beat"));
	CHECK(main->cues[0].pos == CuePos::kFraction);
	CHECK_NEAR(main->cues[0].fraction, 0.5f, 1e-4f);
	// action: named `at:"enter"` -> kEnter.
	CHECK_EQ(main->actions.size(), static_cast<size_t>(1));
	CHECK_EQ(main->actions[0].type, std::string("osf.fade.in"));
	CHECK(main->actions[0].pos == ActionPos::kEnter);
	// sound: `spec` key + numeric `at`.
	CHECK_EQ(main->sounds.size(), static_cast<size_t>(1));
	CHECK_EQ(main->sounds[0].spec, std::string("event:Music"));
	CHECK_EQ(main->sounds[0].role, std::string("lead"));
	CHECK(main->sounds[0].pos == SoundPos::kFraction);
	// camera.
	CHECK_EQ(main->cameras.size(), static_cast<size_t>(1));
	CHECK_EQ(main->cameras[0].state, std::string("thirdperson_hold"));
	CHECK(main->cameras[0].pos == CameraPos::kEnter);
}

OSF_TEST_CASE(Osf_graph_roles_and_policy)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.u.graph");
	CHECK(def != nullptr);
	if (!def) {
		return;
	}
	CHECK_EQ(def->priority, 5);
	CHECK_EQ(def->weight, 2);
	CHECK(def->lockPlayer);
	CHECK(!def->stripActors);  // opted out at the file/scene level
	CHECK_EQ(def->roles.size(), static_cast<size_t>(2));
	CHECK_EQ(def->roles[0].name, std::string("lead"));
	CHECK(def->roles[0].gender == SlotGender::kAny);
	CHECK_EQ(def->roles[1].name, std::string("other"));
	CHECK(def->roles[1].gender == SlotGender::kFemale);
	CHECK(def->playerControl.enabled);
	CHECK(def->playerControl.locked);
}

OSF_TEST_CASE(Osf_rejects_use_xor_stages)
{
	// A node with BOTH use and inline stages is rejected (exactly one allowed).
	CHECK(SceneRegistry::GetSingleton().Find("osf.u.badxor") == nullptr);
	CHECK(AnyErrorContains("both 'use' and 'stages'"));
}

OSF_TEST_CASE(Osf_rejects_reserved_id)
{
	// An authored id containing '#' (the synthetic-node namespace) is rejected.
	CHECK(SceneRegistry::GetSingleton().Find("osf.u.bad#id") == nullptr);
	CHECK(AnyErrorContains("reserved for synthetic stage nodes"));
}

OSF_TEST_CASE(Osf_records_dangling_use)
{
	// The scene parses, but its node `use` resolves to nothing -> recorded at load (scene kept).
	CHECK(SceneRegistry::GetSingleton().Find("osf.u.dangling") != nullptr);
	CHECK(AnyErrorContains("use references unknown scene"));
}

OSF_TEST_CASE(Osf_reports_duplicate_id_first_wins)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.u.dup");
	CHECK(def != nullptr);
	if (def) {
		// _a sorts before _b, so the first-loaded definition (dup_a) survives.
		CHECK_EQ(def->nodes.size(), static_cast<size_t>(1));
		CHECK_EQ(def->nodes[0].stages[0].clips[0].file, std::string("OSF/Anim/dup_a.glb"));
	}
	CHECK(AnyErrorContains("duplicate scene id"));
}

// --- BuildNodePlan: resolve a node (inline stages or `use`) to a playable ScenePlan ---

OSF_TEST_CASE(Osf_build_node_plan_inline_stages_with_role_offsets)
{
	auto& reg = SceneRegistry::GetSingleton();
	const auto* def = reg.Find("osf.u.multistage");
	CHECK(def != nullptr);
	if (!def) {
		return;
	}
	// Node #s0: the play-once first stage (clips a0/b0), 2 roles; role[1] carries a default offset.
	const auto& s0 = def->nodes[0];
	auto plan = reg.BuildNodePlan(*def, s0, 2);
	CHECK(plan.has_value());
	if (plan) {
		CHECK_EQ(plan->stages.size(), static_cast<size_t>(1));
		CHECK_EQ(plan->stages[0].files.size(), static_cast<size_t>(2));
		CHECK_EQ(plan->stages[0].files[0], std::string("OSF/Anim/a0.glb"));
		CHECK_EQ(plan->stages[0].loops, 1);  // play-once stage
		// Role[1]'s default placement (y=1.0) flows into the plan when the clip has no override.
		CHECK_EQ(plan->stages[0].placements.size(), static_cast<size_t>(2));
		CHECK_NEAR(plan->stages[0].placements[1].y, 1.0f, 1e-4f);
	}
}

OSF_TEST_CASE(Osf_build_node_plan_resolves_use_target)
{
	auto& reg = SceneRegistry::GetSingleton();
	const auto* g = reg.Find("osf.u.graph");
	CHECK(g != nullptr);
	if (!g) {
		return;
	}
	const auto* approach = g->FindNode("approach");  // use -> osf.u.shared
	CHECK(approach != nullptr);
	if (approach) {
		auto plan = reg.BuildNodePlan(*g, *approach, 1);
		CHECK(plan.has_value());
		if (plan) {
			CHECK_EQ(plan->stages.size(), static_cast<size_t>(1));
			CHECK_EQ(plan->stages[0].files.size(), static_cast<size_t>(1));
			CHECK_EQ(plan->stages[0].files[0], std::string("OSF/Anim/shared.glb"));
		}
	}
}

OSF_TEST_CASE(Osf_build_node_plan_rejects_wrong_arity)
{
	auto& reg = SceneRegistry::GetSingleton();
	const auto* mini = reg.Find("osf.u.minimal");  // single-role inline scene
	CHECK(mini != nullptr);
	if (mini) {
		CHECK(!reg.BuildNodePlan(*mini, mini->nodes[0], 2).has_value());
	}
	const auto* g = reg.Find("osf.u.graph");
	if (g) {
		const auto* approach = g->FindNode("approach");  // use -> osf.u.shared (single-role)
		if (approach) {
			CHECK(!reg.BuildNodePlan(*g, *approach, 2).has_value());
		}
	}
}

// --- coverage migrated from the legacy *.scene.json suite ---

OSF_TEST_CASE(Osf_camera_state_overrides)
{
	const auto* def = SceneRegistry::GetSingleton().Find("osf.u.camstates");
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

OSF_TEST_CASE(Osf_rejects_unknown_camera_state)
{
	CHECK(SceneRegistry::GetSingleton().Find("osf.u.badcam") == nullptr);
	CHECK(AnyErrorContains("unknown camera state"));
}

OSF_TEST_CASE(Osf_pack_level_camera_attaches_to_entry_node)
{
	// File-level "camera": "vanity_orbit" attaches to the desugared entry node (#s0).
	const auto* def = SceneRegistry::GetSingleton().Find("osf.u.packcam");
	CHECK(def != nullptr);
	if (def) {
		const auto* entry = def->FindNode(def->entry);
		CHECK(entry != nullptr);
		if (entry) {
			CHECK_EQ(entry->cameras.size(), static_cast<size_t>(1));
			CHECK_EQ(entry->cameras[0].state, std::string("vanity_orbit"));
			CHECK(entry->cameras[0].pos == CameraPos::kEnter);
		}
	}
	// A node-level camera track on the entry node wins over the pack default (not overwritten).
	const auto* over = SceneRegistry::GetSingleton().Find("osf.u.packcam.override");
	CHECK(over != nullptr);
	if (over) {
		const auto* fly = over->FindNode("fly");
		CHECK(fly != nullptr);
		if (fly) {
			CHECK_EQ(fly->cameras.size(), static_cast<size_t>(1));
			CHECK_EQ(fly->cameras[0].state, std::string("freefly"));
		}
	}
}

OSF_TEST_CASE(Osf_lock_and_strip_defaults_and_optout)
{
	auto& reg = SceneRegistry::GetSingleton();
	// Defaults: a scene with neither key locks the player and strips apparel.
	const auto* def = reg.Find("osf.u.minimal");
	CHECK(def != nullptr);
	if (def) {
		CHECK(def->lockPlayer);
		CHECK(def->stripActors);
	}
	// Opt-out: lockPlayer:false parses through; stripActors stays default-on.
	const auto* off = reg.Find("osf.u.lockoff");
	CHECK(off != nullptr);
	if (off) {
		CHECK(!off->lockPlayer);
		CHECK(off->stripActors);
	}
}

OSF_TEST_CASE(Osf_records_bad_entry)
{
	CHECK(SceneRegistry::GetSingleton().Find("osf.u.badentry") == nullptr);
	CHECK(AnyErrorContains("is not a node"));
}

OSF_TEST_CASE(Osf_rejects_unresolved_formref)
{
	// Offline, the keyword form-ref can't resolve, so the scene is rejected fail-soft and recorded.
	CHECK(SceneRegistry::GetSingleton().Find("osf.u.formref") == nullptr);
	CHECK(AnyErrorContains("filters.keyword"));
}
