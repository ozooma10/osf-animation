#include "Overlay/RoutePlaybackPlan.h"

namespace OSF::Overlay
{
	namespace
	{
		Animation::PlaybackPlan::Segment BuildLayerSegment(const Registry::RouteLayer& a_layer)
		{
			Animation::PlaybackPlan::Segment segment;
			segment.files = { a_layer.clip.file };
			segment.animIds = { a_layer.clip.animId };
			segment.masks = { a_layer.mask };
			segment.poseModes = { a_layer.mode };
			segment.poseWeights = { a_layer.weight };
			return segment;
		}

		Animation::PlaybackPlan BasePlan(std::string a_id)
		{
			Animation::PlaybackPlan plan;
			plan.animId = std::move(a_id);
			plan.SetWorldPlacementMode(Animation::WorldPlacementMode::kFollowActor);
			return plan;
		}
	}

	std::optional<Animation::PlaybackPlan> BuildRouteStationPlan(
		const Registry::RouteDef& a_route, const Registry::RouteStation& a_station)
	{
		if (!a_station.layer) return std::nullopt;
		auto plan = BasePlan(a_route.id + ":station:" + a_station.id);
		auto stage = BuildLayerSegment(*a_station.layer);
		stage.hold = a_station.layer->holdAt >= 0.0f ? a_station.layer->holdAt : 1.0f;
		plan.stages.push_back(std::move(stage));
		return plan;
	}

	Animation::PlaybackPlan BuildRouteTransitionPlan(const Registry::RouteDef& a_route,
		const Registry::RouteTransition& a_transition, const Registry::RouteStation& a_destination,
		std::uint32_t a_generation)
	{
		auto plan = BasePlan(a_route.id + ":transition:" + a_transition.id);
		auto transitionStage = BuildLayerSegment(a_transition.layer);
		if (a_transition.contactPose) transitionStage.contactPose = { *a_transition.contactPose };
		transitionStage.blendIn = 0.0f;
		transitionStage.loops = 1;
		if (a_transition.commit) {
			transitionStage.marks.push_back({ .seconds = a_transition.commit->frame / Registry::kFrameRate,
				.lane = RouteLane(RouteMarkLane::kCommit), .token = a_transition.commit->id });
		}
		for (std::size_t i = 0; i < a_transition.props.size(); ++i) {
			transitionStage.marks.push_back({ .seconds = a_transition.props[i].frame / Registry::kFrameRate,
				.lane = RouteLane(a_transition.props[i].attach ? RouteMarkLane::kPropAttach : RouteMarkLane::kPropDestroy),
				.token = std::to_string(i) });
		}
		for (std::size_t i = 0; i < a_transition.sounds.size(); ++i) {
			transitionStage.marks.push_back({ .seconds = a_transition.sounds[i].frame / Registry::kFrameRate,
				.lane = RouteLane(RouteMarkLane::kSound), .token = std::to_string(i) });
		}
		for (std::size_t i = 0; i < a_transition.markers.size(); ++i) {
			transitionStage.marks.push_back({ .seconds = a_transition.markers[i].frame / Registry::kFrameRate,
				.lane = RouteLane(RouteMarkLane::kMarker), .token = std::to_string(i) });
		}
		transitionStage.marks.push_back({ .atEnd = true, .lane = RouteLane(RouteMarkLane::kReached),
			.token = std::to_string(a_generation) });
		plan.stages.push_back(std::move(transitionStage));

		if (a_destination.layer) {
			auto hold = BuildLayerSegment(*a_destination.layer);
			hold.hold = a_destination.layer->holdAt >= 0.0f ? a_destination.layer->holdAt : 1.0f;
			plan.stages.push_back(std::move(hold));
		}
		return plan;
	}

}
