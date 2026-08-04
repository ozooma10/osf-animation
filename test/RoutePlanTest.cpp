#include "Check.h"
#include "Animation/PlaybackAdmission.h"
#include "API/OSFOverlayAPI.h"
#include "Overlay/OwnerRegistry.h"
#include "Overlay/RoutePlan.h"

#include <chrono>
#include <future>
#include <type_traits>

using OSF::Test::Check;
using OSF::Test::Finish;

namespace
{
	using BeginMember = OSF::API::OSFOverlayBeginResult (OSF::API::IOSFOverlayAPI::*)(
		std::uint64_t, RE::Actor*, const char*, const char*);
	static_assert(std::is_same_v<decltype(&OSF::API::IOSFOverlayAPI::BeginRoute), BeginMember>);
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
		auto result = InvokeOwnerCallback(owners.BeginDispatch(owner), event, true);
		Check(result.delivered && result.acknowledged && !result.threw && deliveries == 1,
			"owner callbacks acknowledge commit handoffs through a scoped dispatch lease");

		const auto throwing = owners.Acquire("throwing.plugin", ThrowCallback, nullptr);
		result = InvokeOwnerCallback(owners.BeginDispatch(throwing), event, true);
		Check(result.delivered && result.threw && !result.acknowledged && owners.IsUsable(throwing),
			"callback exceptions are isolated without corrupting owner registration");
		Check(owners.Release(throwing), "an exception-throwing owner remains releasable");

		auto lease = owners.BeginDispatch(owner);
		auto released = std::async(std::launch::async, [&]() { return owners.Release(owner); });
		Check(released.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout,
			"owner release waits while a callback lease is active");
		Check(!owners.BeginDispatch(owner), "a releasing owner cannot begin another callback");
		lease = {};
		Check(released.get() && !owners.IsUsable(owner),
			"owner release completes only after the callback barrier is quiescent");
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

	OSF::Registry::RouteTransition Edge(std::string id, std::string from, std::string to,
		OSF::Registry::RouteInterruption interrupt = OSF::Registry::RouteInterruption::kFinish,
		bool commit = false)
	{
		OSF::Registry::RouteTransition edge;
		edge.id = std::move(id);
		edge.from = std::move(from);
		edge.to = std::move(to);
		edge.interruption = interrupt;
		if (commit) edge.commit = OSF::Registry::RouteMarker{ 1.0f, "commit" };
		return edge;
	}

	OSF::Registry::RouteDef Route()
	{
		OSF::Registry::RouteDef route;
		route.id = "test.route";
		for (const char* id : { "a", "b", "c", "d", "isolated" }) route.stations.push_back({ id, std::nullopt });
		route.transitions.push_back(Edge("ab", "a", "b", OSF::Registry::RouteInterruption::kFinish, true));
		route.transitions.push_back(Edge("bd", "b", "d"));
		route.transitions.push_back(Edge("ac", "a", "c", OSF::Registry::RouteInterruption::kCrossfadeBeforeCommit));
		route.transitions.push_back(Edge("cd", "c", "d"));
		route.transitions.push_back(Edge("bc", "b", "c"));
		return route;
	}
}

int main()
{
	using namespace OSF::Overlay;
	CheckOwnerRegistry();
	CheckPlaybackAdmission();
	{
		auto route = Route();
		auto path = ShortestPath(route, "A", "d");
		Check(path.size() == 2 && path[0]->id == "ab" && path[1]->id == "bd",
			"BFS chooses shortest edge count with authored-order tie breaking");
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
			"a request starts one BFS edge only");
		const auto firstGeneration = controller.TransitionGeneration();
		Check(controller.RequestStation("c", 3).disposition == RequestDisposition::kPending && playback.stops == 0,
			"finish interruption records a countermand without replacing the active edge");
		Check(controller.OnCommit(firstGeneration, true) && controller.CheckpointStation() == "b" && controller.ReachedStation() == "a",
			"acknowledged handoff advances the checkpoint before the pose reaches its destination");
		Check(!controller.OnCommit(firstGeneration + 1, true), "stale transition generations are ignored");
		Check(controller.OnEdgeReached(firstGeneration) && controller.ReachedStation() == "b" && playback.last == "bc",
			"edge completion commits reached and reroutes toward the latest desired station");
	}
	{
		auto route = Route();
		FakePlayback playback;
		RouteController controller(route, "a", playback);
		(void)controller.Start();
		(void)controller.RequestStation("c", 10);
		Check(controller.RequestStation("b", 11).disposition == RequestDisposition::kAccepted &&
			playback.stops == 1 && playback.last == "ab",
			"crossfade-before-commit replaces an uncommitted edge and replans from reached");
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
