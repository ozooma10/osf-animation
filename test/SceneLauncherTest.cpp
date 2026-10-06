#include "Check.h"
#include "Scene/SceneLauncher.h"
#include "Scene/AnchorResolve.h"
#include "Animation/GraphManager.h"
#include "UI/HudMessage.h"
#include "Util/Math.h"

using OSF::Test::Check;
using OSF::Scene::SceneRuntime;

namespace
{
	// The production launcher runs against recording runtime/anchor/matchmaking endpoints.
	// Actor/ref identities are opaque; this suite never dereferences game objects.
	int actorStorage[2], anchorStorage;
	auto* const actorA = reinterpret_cast<RE::Actor*>(&actorStorage[0]);
	auto* const actorB = reinterpret_cast<RE::Actor*>(&actorStorage[1]);
	auto* const furniture = reinterpret_cast<RE::TESObjectREFR*>(&anchorStorage);
	const std::vector<RE::Actor*> cast{ actorA, actorB };
	std::shared_ptr<OSF::Registry::ContentRegistrySnapshot> registry;
	struct Calls
	{
		std::vector<std::string> order;
		std::string id, entry, path;
		std::vector<RE::Actor*> actors;
		std::vector<std::string> roles, hud;
		SceneRuntime::AnchorOverride anchor;
		SceneRuntime::StartOverrides overrides;
		RE::TESObjectREFR* anchorRef{};
		std::optional<float> heading;
		bool anchorHud{}, rejectAnchor{};
		std::int32_t handle = 42;
		RE::Actor* speedActor{};
		float speed{};
		std::optional<OSF::Matchmaking::Candidate> pick;
		OSF::Matchmaking::AnchorMode matchMode{};
		OSF::Matchmaking::TagQuery query;
	} calls;

	void Reset(bool a_linear = true, bool a_furniture = false)
	{
		calls = {};
		registry = std::make_shared<OSF::Registry::ContentRegistrySnapshot>();
		OSF::Registry::SceneDef def;
		def.id = "demo";
		def.entry = "authored-entry";
		if (a_linear) def.linearStages = { "stage-zero", "stage-one" };
		if (a_furniture) def.anchorBaseForms.push_back(123);
		registry->scenes.emplace(def.id, std::move(def));
	}

	std::int32_t Started(std::string_view a_path, std::string_view a_id,
		const std::vector<RE::Actor*>& a_actors, const SceneRuntime::AnchorOverride& a_anchor,
		const SceneRuntime::StartOverrides& a_over, std::string_view a_entry)
	{
		calls.order.emplace_back("start");
		calls.path = a_path; calls.id = a_id; calls.actors = a_actors;
		calls.anchor = a_anchor; calls.overrides = a_over; calls.entry = a_entry;
		return calls.handle;
	}
}

namespace OSF::Registry
{
	ContentRegistry& ContentRegistry::GetSingleton() { static ContentRegistry instance; return instance; }
	SceneRef ContentRegistry::Find(std::string_view a_id) const
	{
		SceneRef ref;
		if (const auto it = registry->scenes.find(std::string(a_id)); it != registry->scenes.end()) {
			ref.owner = registry;
			ref.value = &it->second;
		}
		return ref;
	}
}

namespace OSF::Scene
{
	SceneRuntime& SceneRuntime::GetSingleton() { static SceneRuntime instance; return instance; }
	std::int32_t SceneRuntime::StartFromDef(std::string_view a_id, const std::vector<RE::Actor*>& a_actors,
		const StartOverrides& a_over, std::string_view a_entry)
	{
		return Started("positional", a_id, a_actors, {}, a_over, a_entry);
	}
	std::int32_t SceneRuntime::StartFromDefAt(std::string_view a_id, const std::vector<RE::Actor*>& a_actors,
		RE::NiPoint3 a_pos, float a_heading, const StartOverrides& a_over, std::string_view a_entry)
	{
		return Started("anchored", a_id, a_actors, { true, a_pos, a_heading }, a_over, a_entry);
	}
	std::int32_t SceneRuntime::StartFromDefRoles(std::string_view a_id, const std::vector<RE::Actor*>& a_actors,
		const std::vector<std::string>& a_roles, const AnchorOverride& a_anchor,
		const StartOverrides& a_over, std::string_view a_entry)
	{
		calls.roles = a_roles;
		return Started("roles", a_id, a_actors, a_anchor, a_over, a_entry);
	}
	SceneRuntime::AnchorOverride MakeAnchorAt(RE::TESObjectREFR*, std::optional<float>) { return {}; }
	std::optional<SceneRuntime::AnchorOverride> ResolveSceneAnchor(std::string_view, RE::TESObjectREFR* a_ref,
		std::optional<float> a_heading, bool a_hud)
	{
		calls.order.emplace_back("anchor");
		calls.anchorRef = a_ref; calls.heading = a_heading; calls.anchorHud = a_hud;
		if (!a_ref || calls.rejectAnchor) return std::nullopt;
		return SceneRuntime::AnchorOverride{ true, { 11.0f, 22.0f, 33.0f }, a_heading.value_or(0.75f) };
	}
}

namespace OSF::Animation
{
	GraphManager& GraphManager::GetSingleton() { static GraphManager instance; return instance; }
	bool GraphManager::SetSpeed(RE::Actor* a_actor, float a_speed)
	{
		calls.order.emplace_back("speed"); calls.speedActor = a_actor; calls.speed = a_speed;
		return true;
	}
}

namespace OSF::Matchmaking
{
	std::optional<Candidate> Pick(const std::vector<RE::Actor*>&, const TagQuery& a_query,
		RE::TESObjectREFR* a_anchor, AnchorMode a_mode)
	{
		calls.order.emplace_back("match"); calls.anchorRef = a_anchor; calls.matchMode = a_mode; calls.query = a_query;
		return calls.pick;
	}
}

namespace OSF::UI::HudMessage
{
	void Error(std::string_view a_text) { calls.hud.emplace_back(a_text); }
}

int main()
{
	using namespace OSF::Scene;
	constexpr auto tag = "[Scene] launch test";
	Reset();
	Check(LaunchDef("missing", cast, {}, tag) == 0 && calls.order.empty(), "missing scenes never start");
	Check(LaunchDef("demo", {}, {}, tag) == 0 && calls.order.empty(), "empty casts never start");
	Check(LaunchDef("demo", { actorA, nullptr }, {}, tag) == 0 && calls.order.empty(), "null actors never reach the runtime");

	for (const auto stage : { -1, 0, 1 }) {
		Reset();
		DefLaunchOptions opts;
		opts.stage = stage;
		Check(LaunchDef("demo", cast, opts, tag) == 42 && calls.path == "positional" && calls.actors == cast,
			"positional launch preserves cast order and handle");
		Check(calls.entry == (stage == 1 ? "stage-one" : ""), "stage selection happens before start; zero preserves authored entry");
	}
	Reset();
	DefLaunchOptions opts;
	opts.stage = 2;
	Check(LaunchDef("demo", cast, opts, tag) == 0 && calls.order.empty() && calls.hud.size() == 1,
		"Papyrus out-of-range stage refuses playback and reports the error");
	Reset();
	opts.invalidStage = InvalidStagePolicy::kUseEntry;
	opts.emitHud = false;
	Check(LaunchDef("demo", cast, opts, tag) == 42 && calls.entry.empty() && calls.hud.empty(),
		"native out-of-range stage preserves entry fallback without a HUD popup");
	for (const auto policy : { InvalidStagePolicy::kReject, InvalidStagePolicy::kUseEntry }) {
		Reset(false);
		opts.invalidStage = policy;
		Check(LaunchDef("demo", cast, opts, tag) == 42 && calls.entry.empty(), "both adapters ignore stage on non-linear scenes");
	}

	Reset();
	opts = {};
	opts.worldAnchor = { true, { 1, 2, 3 }, 0.5f };
	Check(LaunchDef("demo", cast, opts, tag) == 42 && calls.path == "anchored" && calls.anchor.pos.x == 1 &&
		calls.anchor.heading == 0.5f && calls.order == std::vector<std::string>{ "start" }, "free scenes accept explicit world coordinates");
	Reset(true, true);
	Check(LaunchDef("demo", cast, opts, tag) == 0 && calls.order == std::vector<std::string>{ "anchor" },
		"world coordinates cannot bypass a scene's furniture requirement");
	Reset(true, true);
	opts.anchorRef = furniture;
	opts.headingRad = 1.25f;
	opts.emitHud = false;
	Check(LaunchDef("demo", cast, opts, tag) == 42 && calls.anchorRef == furniture && calls.heading == 1.25f &&
		!calls.anchorHud && calls.anchor.pos.x == 11 && calls.anchor.heading == 1.25f,
		"reference resolution wins over world coordinates and forwards heading and HUD policy");
	Reset(true, true);
	calls.rejectAnchor = true;
	opts.speed = 2.0f;
	Check(LaunchDef("demo", cast, opts, tag) == 0 && calls.order == std::vector<std::string>{ "anchor" },
		"rejected furniture never starts or changes playback speed");

	Reset();
	opts = {};
	opts.stage = 1;
	opts.speed = 1.5f;
	opts.overrides.hideApparel = false;
	opts.overrides.camera = "none";
	const std::vector<std::string> roles{ "second", "first" };
	Check(LaunchDef("demo", cast, opts, tag, &roles) == 42 && calls.path == "roles" && calls.roles == roles &&
		calls.actors == cast && calls.entry == "stage-one" && calls.overrides.hideApparel == false && calls.overrides.camera == "none",
		"named launch forwards names, cast, stage and overrides to the runtime's role binder");
	Check(calls.order == std::vector<std::string>{ "start", "speed" } && calls.speedActor == actorA && calls.speed == 1.5f,
		"native speed override is applied only after start, using the original first actor");
	Reset();
	calls.handle = 0;
	Check(LaunchDef("demo", cast, opts, tag) == 0 && calls.order == std::vector<std::string>{ "start" },
		"runtime start failure does not change an actor's speed");
	Reset();
	opts.speed = 1.0f;
	const std::vector<std::string> emptyRoles;
	Check(LaunchDef("demo", cast, opts, tag, &emptyRoles) == 42 && calls.path == "roles" && calls.roles.empty() &&
		calls.order == std::vector<std::string>{ "start" }, "empty named casts still reach role validation; default speed leaves authored speed alone");

	Reset();
	LaunchOpts papyrus;
	papyrus.stage = 1; papyrus.speed = 2; papyrus.blendIn = 3; papyrus.anchor = furniture; papyrus.headingDeg = 90;
	papyrus.hideApparelMode = 0; papyrus.playerInputLockMode = 1; papyrus.sceneControlsMode = 0;
	papyrus.fadeMode = 1; papyrus.followActorMode = 1; papyrus.camera = "none"; papyrus.loopScale = 2;
	const auto launch = MakeDefLaunchOptions(papyrus);
	Check(LaunchDef("demo", cast, launch, tag) == 42 && calls.entry == "stage-one" &&
		calls.order == std::vector<std::string>{ "anchor", "start" }, "Papyrus registered starts keep authored speed and use the requested stage");
	Check(calls.heading && std::abs(*calls.heading - 90 * OSF::Util::kDegToRadF) < 0.0001f && calls.anchorHud &&
		calls.overrides.hideApparel == false && calls.overrides.playerInputLock == true && calls.overrides.sceneControls == false &&
		calls.overrides.fade == true && calls.overrides.worldPlacement == OSF::Animation::WorldPlacementMode::kFollowActor &&
		calls.overrides.camera == "none" && calls.overrides.loopScale == 2,
		"Papyrus options preserve units, tri-states, placement, camera and loop scaling");

	Reset();
	Check(LaunchMatched(cast, {}, papyrus, tag) == 0 && calls.order == std::vector<std::string>{ "match" },
		"no matchmaking result cannot start a scene");
	Reset();
	calls.pick = OSF::Matchmaking::Candidate{ "demo", 0, 1, { 1, 0 } };
	OSF::Matchmaking::TagQuery query{ { "all" }, { "any" }, { "none" } };
	Check(LaunchMatched(cast, query, papyrus, tag, OSF::Matchmaking::AnchorMode::kRequire) == 42 &&
		calls.actors == std::vector<RE::Actor*>{ actorB, actorA } && calls.entry.empty() &&
		calls.order == std::vector<std::string>{ "match", "anchor", "start" },
		"matchmade starts preserve picked cast order, authored entry and authored speed");
	Check(calls.matchMode == OSF::Matchmaking::AnchorMode::kRequire && calls.anchorRef == furniture &&
		calls.query.allOf == query.allOf && calls.query.anyOf == query.anyOf && calls.query.noneOf == query.noneOf,
		"matchmade starts preserve furniture and tag filters");
	return OSF::Test::Finish("scene launcher");
}
