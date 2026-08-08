#include "Check.h"
#include "Animation/PlaybackAdmission.h"
#include "API/OSFOverlayAPI.h"
#include "Overlay/OwnerRegistry.h"
#include "Overlay/RoutePlan.h"
#include "Overlay/RoutePlaybackPlan.h"
#include "Scene/RouteInspectionTimeline.h"

#include <type_traits>

using OSF::Test::Check;
using OSF::Test::Finish;

namespace
{
	using BeginMember = OSF::API::OSFOverlayBeginResult (OSF::API::IOSFOverlayAPI::*)(
		std::uint64_t, RE::Actor*, const char*, const char*);
	using RouteForActorMember = std::int32_t (OSF::API::IOSFOverlayAPI::*)(RE::Actor*);
	using QueryMember = bool (OSF::API::IOSFOverlayAPI::*)(
		std::int32_t, OSF::API::OSFOverlayRouteState&);
	static_assert(std::is_same_v<decltype(&OSF::API::IOSFOverlayAPI::BeginRoute), BeginMember>);
	static_assert(std::is_same_v<decltype(&OSF::API::IOSFOverlayAPI::GetRouteForActor), RouteForActorMember>);
	static_assert(std::is_same_v<decltype(&OSF::API::IOSFOverlayAPI::QueryRoute), QueryMember>);
	static_assert(std::is_standard_layout_v<OSF::API::OSFOverlayBeginResult>);
	static_assert(std::is_trivially_copyable_v<OSF::API::OSFOverlayBeginResult>);
	static_assert(OSF::API::OverlayEventType::kPropAttach != OSF::API::OverlayEventType::kMarker);
	static_assert(OSF::API::OverlayEventType::kPropDestroy != OSF::API::OverlayEventType::kMarker);

	// Compiled but deliberately not run: this is the same surface a tiny SFSE consumer sees.
	[[maybe_unused]] OSF::API::OSFOverlayBeginResult ConsumeOverlayHeader(
		OSF::API::IOSFOverlayAPI& a_api, std::uint64_t a_owner, RE::Actor* a_actor)
	{
		auto result = a_api.BeginRoute(a_owner, a_actor, "consumer.route", "idle");
		if (result.routeHandle != 0) {
			(void)a_api.RequestStation(result.routeHandle, "active", 1);
			(void)a_api.EndRoute(result.routeHandle);
		}
		return result;
	}

	bool Acknowledge(const OSF::API::OSFOverlayEvent* a_event, void* a_context)
	{
		auto* deliveries = static_cast<int*>(a_context);
		++*deliveries;
		return a_event && a_event->type == OSF::API::OverlayEventType::kCommit;
	}

	bool ThrowCallback(const OSF::API::OSFOverlayEvent*, void*)
	{
		throw std::runtime_error("consumer callback failure");
	}

	void CheckOwnerRegistry()
	{
		using namespace OSF::Overlay;
		OwnerRegistry owners;
		int deliveries = 0;
		const auto owner = owners.Acquire("Test.Plugin", Acknowledge, &deliveries);
		Check(owner != 0 && owners.Acquire("test.plugin", Acknowledge, &deliveries) == 0,
			"callback owners are case-insensitively unique");

		OSF::API::OSFOverlayEvent event;
		event.type = OSF::API::OverlayEventType::kCommit;
		auto result = InvokeOwnerCallback(owners.GetCallback(owner), event, true);
		Check(result.acknowledged && !result.threw && deliveries == 1,
			"owner callbacks acknowledge commit handoffs");

		const auto throwing = owners.Acquire("throwing.plugin", ThrowCallback, nullptr);
		result = InvokeOwnerCallback(owners.GetCallback(throwing), event, true);
		Check(result.threw && !result.acknowledged && owners.IsUsable(throwing),
			"callback exceptions are isolated without corrupting owner registration");
		Check(owners.Release(throwing), "an exception-throwing owner remains releasable");
		Check(owners.Release(owner) && !owners.IsUsable(owner) && !owners.GetCallback(owner),
			"owner release removes its callback record");
	}

	void CheckRoutePlaybackPlans()
	{
		using namespace OSF;
		Registry::RouteDef route;
		route.id = "test.plan";
		route.stations.push_back({ "zero", std::nullopt });
		Registry::RouteLayer holdLayer;
		holdLayer.clip.file = "hold.af";
		holdLayer.mask = "upperBody";
		holdLayer.holdAt = 0.75f;
		route.stations.push_back({ "held", holdLayer });

		Registry::RouteTransition transition;
		transition.id = "zero-held";
		transition.from = "zero";
		transition.to = "held";
		transition.layer.clip.file = "transition.af";
		transition.layer.mask = "arms";
		transition.commit = Registry::RouteMarker{ 10.0f, "commit" };
		transition.props = {
			{ .frame = 10.0f, .id = "new", .attach = true },
			{ .frame = 10.0f, .id = "old", .attach = false },
		};
		Animation::ContactPose contact;
		contact.enabled = true;
		contact.atSeconds = 10.0f / Registry::kFrameRate;
		contact.bones = { "C_Head" };
		transition.contactPose = contact;

		Check(!Overlay::BuildRouteStationPlan(route, route.stations[0]),
			"zero-animation stations compile to no playback plan");
		const auto station = Overlay::BuildRouteStationPlan(route, route.stations[1]);
		Check(station && station->stages.size() == 1 && station->stages[0].hold == 0.75f &&
			station->masks.empty() && station->poseModes.empty() && station->poseWeights.empty(),
			"station plans keep pose policy only on their stage");

		const auto runtime = Overlay::BuildRouteTransitionPlan(route, transition, route.stations[1], 7);
		const auto& transitionStage = runtime.stages.front();
		Check(runtime.stages.size() == 2 && transitionStage.contactPose.size() == 1 &&
			transitionStage.marks.size() == 4 && transitionStage.blendIn == 0.0f,
			"runtime transition plans start at authored weight and carry contact pose, side effects, reached mark, and destination hold");
		const auto laneAt = [&](std::size_t index) { return transitionStage.marks[index].lane; };
		Check(laneAt(1) < laneAt(0) && laneAt(0) < laneAt(2),
			"same-frame prop replacement attaches before commit and destroys after acknowledgement");

		const auto preview = Overlay::BuildRouteTransitionPreviewPlan(route, transition);
		Check(preview.speed == 0.0f && preview.stages.size() == 1 && preview.stages[0].marks.empty() &&
			preview.stages[0].contactPose.size() == 1 && preview.stages[0].blendIn == 0.0f,
			"route preview reuses production pose compilation without scheduling side effects");
	}

	void CheckPlaybackAdmission()
	{
		using namespace OSF::Animation;
		Check(EvaluatePlaybackAdmission({}, 0, 10).accepted,
			"an empty actor admits a new staged owner");
		Check(EvaluatePlaybackAdmission({}, 99, 10).reason == PlaybackAdmissionReason::kExpectedMissing,
			"replacement requires the named playback to exist");
		Check(EvaluatePlaybackAdmission({ PlaybackOccupant::kStandalone, 0, 0 }, 0, 10).reason ==
			PlaybackAdmissionReason::kStandaloneOccupied,
			"a service-owned staged request cannot clobber standalone playback");
		Check(EvaluatePlaybackAdmission({ PlaybackOccupant::kStandalone, 0, 0 }, 0, 0).accepted,
			"the legacy sink may retain its explicit standalone replacement behavior");
		const PlaybackClaim staged{ PlaybackOccupant::kStaged, 42, 7 };
		Check(EvaluatePlaybackAdmission(staged, 0, 7).reason == PlaybackAdmissionReason::kPlaybackMismatch,
			"a staged playback is never replaced without its exact id");
		Check(EvaluatePlaybackAdmission(staged, 42, 8).reason == PlaybackAdmissionReason::kOwnerMismatch,
			"a playback id cannot be used by a different registered sink");
		const auto owned = EvaluatePlaybackAdmission(staged, 42, 7);
		Check(owned.accepted && owned.replace, "the exact playback and sink can replace their own graph");

		std::vector<TimedMark> marks{ TimedMark{ .seconds = 0.5f, .token = "inside" } };
		Check(FirstInvalidStrictTimedMark(marks, 1.0f) == nullptr,
			"strict authored marks inside the clip are admitted");
		marks.push_back(TimedMark{ .seconds = 1.0f, .token = "at-end" });
		Check(FirstInvalidStrictTimedMark(marks, 1.0f) == &marks.back(),
			"strict authored marks at the clip duration are rejected before graph mutation");
		marks.back().atEnd = true;
		Check(FirstInvalidStrictTimedMark(marks, 1.0f) == nullptr,
			"the dedicated at-end lane remains valid");
	}

	struct FakePlayback final : OSF::Overlay::IRoutePlayback
	{
		bool PlayStation(const OSF::Registry::RouteStation& a_station) override
		{
			++stations;
			last = a_station.id;
			return !fail;
		}
		bool PlayTransition(const OSF::Registry::RouteTransition& a_transition,
			const OSF::Registry::RouteStation&, std::uint32_t a_generation) override
		{
			++transitions;
			last = a_transition.id;
			generation = a_generation;
			return !fail;
		}
		void Stop(bool) override { ++stops; }

		int stations = 0;
		int transitions = 0;
		int stops = 0;
		bool fail = false;
		std::string last;
		std::uint32_t generation = 0;
	};

	OSF::Registry::RouteTransition Transition(std::string id, std::string from, std::string to,
		OSF::Registry::RouteInterruption interrupt = OSF::Registry::RouteInterruption::kFinish,
		bool commit = false)
	{
		OSF::Registry::RouteTransition transition;
		transition.id = std::move(id);
		transition.from = std::move(from);
		transition.to = std::move(to);
		transition.interruption = interrupt;
		if (commit) transition.commit = OSF::Registry::RouteMarker{ 1.0f, "commit" };
		return transition;
	}

	OSF::Registry::RouteDef Route()
	{
		OSF::Registry::RouteDef route;
		route.id = "test.route";
		for (const char* id : { "a", "b", "c", "d", "isolated" }) route.stations.push_back({ id, std::nullopt });
		route.transitions.push_back(Transition("ab", "a", "b", OSF::Registry::RouteInterruption::kFinish, true));
		route.transitions.push_back(Transition("bd", "b", "d"));
		route.transitions.push_back(Transition("ac", "a", "c", OSF::Registry::RouteInterruption::kCrossfadeBeforeCommit));
		route.transitions.push_back(Transition("cd", "c", "d"));
		route.transitions.push_back(Transition("bc", "b", "c"));
		return route;
	}
}

int main()
{
	using namespace OSF::Overlay;
	CheckOwnerRegistry();
	CheckPlaybackAdmission();
	CheckRoutePlaybackPlans();
	{
		OSF::Animation::ContactPose pose{
			.enabled = true,
			.atSeconds = 1.0f,
			.approachSeconds = 0.4f,
			.fullBeforeSeconds = 0.1f,
			.fullAfterSeconds = 0.1f,
			.releaseSeconds = 0.4f,
		};
		Check(pose.WeightAt(0.6f) == 0.0f && pose.WeightAt(0.9f) == 1.0f &&
			pose.WeightAt(1.1f) == 1.0f && pose.WeightAt(1.5f) == 0.0f,
			"contact pose is exactly zero outside its authored window and full across contact");
		Check(std::abs(pose.WeightAt(0.75f) - 0.5f) < 0.0001f &&
			std::abs(pose.WeightAt(1.3f) - 0.5f) < 0.0001f,
			"contact pose uses symmetric smoothstep midpoints");
	}
	{
		OSF::Registry::RouteTransition transition;
		transition.props = {
			{ .frame = 2.0f, .id = "carrier", .attach = true, .lifetime = OSF::Registry::RouteLifetime::kTransition },
			{ .frame = 3.0f, .id = "consumer", .attach = true, .lifetime = OSF::Registry::RouteLifetime::kExternal },
			{ .frame = 4.0f, .id = "station", .attach = true, .lifetime = OSF::Registry::RouteLifetime::kStation },
			{ .frame = 6.0f, .id = "carrier", .attach = false, .lifetime = OSF::Registry::RouteLifetime::kTransition },
		};
		auto at1 = OSF::Scene::InspectionRoutePropsAt(transition, 1.0f, false);
		auto at4 = OSF::Scene::InspectionRoutePropsAt(transition, 4.0f, false);
		auto atEnd = OSF::Scene::InspectionRoutePropsAt(transition, 4.0f, true);
		Check(at1.empty(), "route debugger starts before authored prop marks");
		Check(at4.size() == 2 && at4[0]->id == "carrier" && at4[1]->id == "station",
			"route debugger reconstructs authored OSF-owned props and excludes external callbacks");
		Check(atEnd.size() == 1 && atEnd[0]->id == "station",
			"route debugger applies transition-reached cleanup to transition-lifetime props at clip end");
		Check(OSF::Scene::InspectionRoutePropsAt(transition, 6.0f, false).size() == 1,
			"route debugger rewinds attach and destroy marks deterministically");
		transition.props.push_back({ .frame = 10.0f, .id = "late", .attach = true,
			.lifetime = OSF::Registry::RouteLifetime::kStation });
		Check(OSF::Scene::InspectionRoutePropsAt(transition, 10.0f, false, 10.0f).size() == 1,
			"route debugger does not materialize an atFrame prop at the strict decoded clip end");
	}
	{
		auto route = Route();
		auto path = ShortestPath(route, "A", "d");
		Check(path.size() == 2 && path[0]->id == "ab" && path[1]->id == "bd",
			"BFS chooses shortest transition count with authored-order tie breaking");
		Check(ShortestPath(route, "d", "a").empty(), "directed unreachable pairs remain no-path at request time");
	}
	{
		auto route = Route();
		FakePlayback playback;
		RouteController controller(route, "a", playback);
		Check(controller.Start().disposition == RequestDisposition::kAccepted && playback.stations == 1,
			"controller starts at a zero-animation station");
		Check(controller.RequestStation("a", 1).disposition == RequestDisposition::kAccepted && playback.stations == 1,
			"reasserting the reached station is idempotent");
		Check(controller.RequestStation("d", 2).disposition == RequestDisposition::kAccepted && playback.last == "ab",
			"a request starts one BFS transition only");
		const auto firstGeneration = controller.TransitionGeneration();
		Check(controller.RequestStation("c", 3).disposition == RequestDisposition::kPending && playback.stops == 0,
			"finish interruption records a countermand without replacing the active transition");
		Check(controller.OnCommit(firstGeneration, true) && controller.CheckpointStation() == "b" && controller.ReachedStation() == "a",
			"acknowledged handoff advances the checkpoint before the pose reaches its destination");
		Check(!controller.OnCommit(firstGeneration + 1, true), "stale transition generations are ignored");
		Check(controller.OnTransitionReached(firstGeneration) && controller.ReachedStation() == "b" && playback.last == "bc",
			"transition completion commits reached and reroutes toward the latest desired station");
	}
	{
		auto route = Route();
		FakePlayback playback;
		RouteController controller(route, "a", playback);
		(void)controller.Start();
		(void)controller.RequestStation("c", 10);
		Check(controller.RequestStation("b", 11).disposition == RequestDisposition::kAccepted &&
			playback.stops == 1 && playback.last == "ab",
			"crossfade-before-commit replaces an uncommitted transition and replans from reached");
	}
	{
		auto route = Route();
		FakePlayback playback;
		RouteController controller(route, "a", playback);
		(void)controller.Start();
		controller.SetBlocker(ControllerBlocker::kScene);
		Check(controller.RequestStation("d", 20).reason == RequestReason::kSceneBlocked,
			"scene suspension keeps requests pending");
		controller.ClearBlocker(ControllerBlocker::kScene);
		Check(controller.Phase() == ControllerPhase::kTransitioning && playback.last == "ab",
			"scene resume reconciles from the ownership-safe checkpoint");
		controller.SetBlocker(ControllerBlocker::kActor3D);
		Check(controller.ReachedStation() == controller.CheckpointStation() && playback.stops >= 2,
			"3D loss stops playback and resumes from checkpoint state");
	}
	{
		auto route = Route();
		FakePlayback playback;
		RouteController controller(route, "a", playback);
		(void)controller.Start();
		Check(controller.RequestStation("isolated", 30).reason == RequestReason::kNoPath,
			"an unreachable destination returns structured noPath");
		(void)controller.RequestStation("b", 31);
		const auto generation = controller.TransitionGeneration();
		Check(controller.OnCommit(generation, false) && controller.Phase() == ControllerPhase::kFailed,
			"a rejected handoff fails and cleans the controller");
	}
	return Finish("route plan");
}
