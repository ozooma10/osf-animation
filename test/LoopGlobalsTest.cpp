#include "Check.h"
#include "Registry/ContentRegistry.h"
#include "Scene/SceneTiming.h"
#include "Util/FormRef.h"

#include <fstream>
#include <limits>

namespace
{
	using OSF::Test::Check;
	using namespace OSF::Registry;

	std::optional<OSF::Animation::PlaybackPlan> Plan(const SceneRef& def, std::size_t node = 0, float scale = 1)
	{
		if (!Check(def && node < def->nodes.size(), "resolved definition contains requested node")) return {};
		auto plan = ContentRegistry::GetSingleton().BuildNodePlan(def, def->nodes[node], 1);
		if (!Check(plan.has_value(), "resolved definition builds a playback plan")) return {};
		OSF::Scene::ApplyNodeTiming(*plan, def->nodes[node], scale);
		return plan;
	}
}

void CheckLoopGlobals()
{
	struct RestoreDirectory
	{
		std::filesystem::path previous = std::filesystem::current_path();
		~RestoreDirectory() { std::filesystem::current_path(previous); }
	} restore;
	std::filesystem::current_path(restore.previous.parent_path() / "loop-globals");
	auto& reg = ContentRegistry::GetSingleton();
	reg.LoadAll();

	nlohmann::json fixture;
	std::ifstream("Data/OSF/loop-globals.osf.json") >> fixture;
	std::size_t rejected = 0;
	for (const auto& scene : fixture.at("scenes")) {
		const auto id = scene.at("id").get<std::string>();
		if (id.starts_with("invalid.")) {
			++rejected;
			Check(!reg.Find(id), "reject malformed loop binding: " + id);
		} else Check(static_cast<bool>(reg.Find(id)), "load loop fixture: " + id);
	}
	Check(reg.LoadErrors().size() == rejected, "only malformed bindings produce load errors; missing plugins are allowed");

	const auto syntax = OSF::Util::ParseFormRef("Optional.esm|0x1234");
	Check(syntax && syntax->plugin == "Optional.esm" && syntax->local == 0x1234,
		"form-reference syntax can be validated without a loaded plugin");
	const auto full = OSF::Util::ParseFormRef("Optional.esm|FD121234");
	Check(full && full->local == 0xFD121234, "existing full-form-ID and prefix-free hexadecimal syntax remains supported");

	const auto authored = reg.Find("global.linear");
	if (!authored) return;
	float setting = 5;
	int reads = 0;
	const ContentRegistry::GlobalReader reader = [&](const std::string&) -> std::optional<float> {
		++reads;
		return setting;
	};
	const auto first = reg.ResolveLoopGlobals(authored, reader);
	Check(reads == 1, "one global shared by stages and synthetic nodes is read once per start");
	Check(first.get() != authored.get() && authored->nodes[0].loopCount == 3 && authored->nodes[1].loopCount == 0,
		"resolving a start leaves authored defaults and registry definitions unchanged");
	Check(reg.Find("global.linear").get() == authored.get(), "a resolved definition is never published to the catalog");
	if (const auto plan = Plan(first)) Check(plan->stages[0].loops == 5, "linear stage receives the global count");
	if (const auto plan = Plan(first, 1)) {
		Check(plan->stages[0].loops == 5 && plan->stages[0].timer == 20,
			"a zero fallback still supports a positive runtime count alongside its timer");
		Check(std::ranges::any_of(first->nodes[1].edges, [](const auto& edge) { return edge.when == EdgeWhen::kLoops; }),
			"linear desugaring retains a loop-completion edge even when the fallback is zero");
	}
	setting = 2;
	if (const auto plan = Plan(first, 1)) Check(plan->stages[0].loops == 5, "later stages retain the scene-start value");
	const auto second = reg.ResolveLoopGlobals(authored, reader);
	if (const auto plan = Plan(second)) Check(plan->stages[0].loops == 2, "a new start reads the changed global without reload");
	Check(reads == 2, "node revisits and playback-plan builds never reread globals");
	Check(reg.ResolveLoopGlobals(first, reader).get() == first.get() && reads == 2,
		"an already resolved snapshot remains frozen");
	Check(reg.ResolveLoopGlobals(reg.Find("global.literal"), reader).get() == reg.Find("global.literal").get() && reads == 2,
		"literal-only content keeps its original snapshot and performs no global reads");
	Check(!reg.ResolveLoopGlobals({}, reader), "invalid scene refs resolve without game access");

	for (const auto value : { std::optional<float>{}, std::optional<float>{ -1.0f }, std::optional<float>{ 2.5f },
		std::optional<float>{ std::numeric_limits<float>::quiet_NaN() },
		std::optional<float>{ std::numeric_limits<float>::infinity() },
		std::optional<float>{ 2147483648.0f } }) {
		int missingReads = 0;
		const auto fallback = reg.ResolveLoopGlobals(authored, [&](const std::string&) {
			++missingReads;
			return value;
		});
		Check(missingReads == 1, "missing or invalid global results are also captured once");
		if (const auto plan = Plan(fallback)) Check(plan->stages[0].loops == 3, "missing/invalid global uses the field's fallback");
		if (const auto plan = Plan(fallback, 1)) Check(plan->stages[0].loops == 0 && plan->stages[0].timer == 20,
			"the same unavailable global can have different defaults on different stages");
	}

	const auto zero = reg.ResolveLoopGlobals(authored, [](const std::string&) { return 0.0f; });
	if (const auto plan = Plan(zero, 0, 2)) {
		Check(plan->stages[0].loops == 0 && zero->nodes[0].loopMode == LoopMode::kHold,
			"zero is an intentional hold and is not multiplied into a finite count");
		OSF::Animation::PlaybackSession session;
		OSF::Animation::PlaybackSession::SegmentData stage;
		stage.duration = 1; stage.loops = plan->stages[0].loops;
		session.stages.push_back(stage);
		Check(session.SetSegment(0), "zero-count transport initializes");
		int owner = 0;
		(void)session.Advance(&owner, 10);
		Check(!session.ended, "a global-backed zero count holds across repeated animation wraps");
	}
	if (const auto plan = Plan(first, 0, 2)) {
		Check(plan->stages[0].loops == 10, "LoopScale multiplies the captured count");
		OSF::Animation::PlaybackSession session;
		OSF::Animation::PlaybackSession::SegmentData stage;
		stage.duration = 1; stage.loops = plan->stages[0].loops;
		session.stages.push_back(stage);
		Check(session.SetSegment(0), "counted transport initializes");
		int owner = 0;
		(void)session.Advance(&owner, 9.5f);
		Check(!session.ended, "resolved/scaled playback remains active before its last loop");
		(void)session.Advance(&owner, 0.5f);
		Check(session.ended && session.endReason == OSF::Animation::PlaybackEndReason::kLoopComplete,
			"resolved/scaled playback completes at the requested count");
	}
	if (const auto plan = Plan(first, 0, 0.5f)) Check(plan->stages[0].loops == 3, "fractional LoopScale rounds the resolved count");
	if (const auto plan = Plan(first, 0, 0.01f)) Check(plan->stages[0].loops == 1, "a positive count is floored at one after scaling");
	const auto large = reg.ResolveLoopGlobals(authored, [](const std::string&) { return 2147483520.0f; });
	if (const auto plan = Plan(large, 0, 20)) Check(plan->stages[0].loops == std::numeric_limits<std::int32_t>::max(),
		"scaling a large valid global saturates without integer overflow");

	for (const auto id : { "global.graph", "global.inline" }) {
		std::vector<std::string> queried;
		const auto resolved = reg.ResolveLoopGlobals(reg.Find(id), [&](const std::string& ref) -> std::optional<float> {
			queried.push_back(ref);
			return ref.ends_with("1235") ? 4.0f : 7.0f;
		});
		Check(queried.size() == 2, "graph and referenced inline-stage globals are captured once each");
		if (const auto plan = Plan(resolved, 0, 2)) {
			Check(plan->stages.size() == 2 && plan->stages[0].loops == 8 && plan->stages[1].loops == 14,
				"graph node count controls its terminal stage; earlier stages use their own globals");
		}
	}
	const auto graphZero = reg.Find("global.zero");
	Check(graphZero && graphZero->nodes[0].loopMode == LoopMode::kHold, "global-backed graph accepts a loops edge and retains its zero default for catalog reads");
	const auto graphPositive = reg.ResolveLoopGlobals(graphZero, [](const std::string&) { return 6.0f; });
	if (const auto plan = Plan(graphPositive)) Check(plan->stages[0].loops == 6 && graphPositive->nodes[0].loopMode == LoopMode::kCount,
		"global-backed graph retains counted-loop routing for a positive value");
	const auto graphFallback = reg.ResolveLoopGlobals(graphZero, [](const std::string&) { return std::optional<float>{}; });
	if (const auto plan = Plan(graphFallback)) Check(plan->stages[0].loops == 0 && graphFallback->nodes[0].loopMode == LoopMode::kHold,
		"global-backed graph defaults to hold when its global is unavailable");

	const auto literal = reg.Find("global.literal");
	if (const auto plan = Plan(literal)) Check(plan->stages[0].loops == 3, "literal integer loops are unchanged");
	if (const auto plan = Plan(literal, 1)) Check(plan->stages[0].loops == 0, "literal zero holds are unchanged");
	if (const auto plan = Plan(literal, 2, 2)) Check(plan->stages[0].loops == 0 && plan->stages[0].timer == 2,
		"timer-only stages remain unaffected by LoopScale");

	// Reload replaces the publication, while old starts and their `use` dependencies stay pinned.
	const auto pinnedGraph = reg.ResolveLoopGlobals(reg.Find("global.graph"), reader);
	reg.LoadAll();
	if (const auto plan = Plan(pinnedGraph)) Check(plan->stages[0].loops == 2 && plan->stages[1].loops == 2,
		"a reload cannot invalidate a captured scene or its referenced stages");
	if (const auto plan = Plan(first)) Check(plan->stages[0].loops == 5, "old starts retain their values across reload");
}
