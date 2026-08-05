#include "Overlay/RoutePlaybackPlan.h"

namespace OSF::Overlay
{
	namespace
	{
		Animation::ScenePlan::Stage BuildLayerStage(const Registry::RouteLayer& a_layer)
		{
			Animation::ScenePlan::Stage stage;
			stage.files = { a_layer.clip.file };
			stage.animIds = { a_layer.clip.animId };
			stage.masks = { a_layer.mask };
			stage.poseModes = { a_layer.mode };
			stage.poseWeights = { a_layer.weight };
			return stage;
		}

		Animation::ScenePlan BasePlan(std::string a_id)
		{
			Animation::ScenePlan plan;
			plan.animId = std::move(a_id);
			plan.anchored = false;
			return plan;
		}
	}

	std::optional<Animation::ScenePlan> BuildRouteStationPlan(
		const Registry::RouteDef& a_route, const Registry::RouteStation& a_station)
	{
		if (!a_station.layer) return std::nullopt;
		auto plan = BasePlan(a_route.id + ":station:" + a_station.id);
		auto stage = BuildLayerStage(*a_station.layer);
		stage.hold = a_station.layer->holdAt >= 0.0f ? a_station.layer->holdAt : 1.0f;
		plan.stages.push_back(std::move(stage));
		return plan;
	}

	Animation::ScenePlan BuildRouteTransitionPlan(const Registry::RouteDef& a_route,
		const Registry::RouteTransition& a_transition, const Registry::RouteStation& a_destination,
		std::uint32_t a_generation)
	{
		auto plan = BasePlan(a_route.id + ":transition:" + a_transition.id);
		auto edge = BuildLayerStage(a_transition.layer);
		if (a_transition.contactPose) edge.contactPose = { *a_transition.contactPose };
		edge.loops = 1;
		if (a_transition.commit) {
			edge.marks.push_back({ .seconds = a_transition.commit->frame / Registry::kFrameRate,
				.lane = RouteLane(RouteMarkLane::kCommit), .token = a_transition.commit->id });
		}
		for (std::size_t i = 0; i < a_transition.props.size(); ++i) {
			edge.marks.push_back({ .seconds = a_transition.props[i].frame / Registry::kFrameRate,
				.lane = RouteLane(a_transition.props[i].attach ? RouteMarkLane::kPropAttach : RouteMarkLane::kPropDestroy),
				.token = std::to_string(i) });
		}
		for (std::size_t i = 0; i < a_transition.sounds.size(); ++i) {
			edge.marks.push_back({ .seconds = a_transition.sounds[i].frame / Registry::kFrameRate,
				.lane = RouteLane(RouteMarkLane::kSound), .token = std::to_string(i) });
		}
		for (std::size_t i = 0; i < a_transition.markers.size(); ++i) {
			edge.marks.push_back({ .seconds = a_transition.markers[i].frame / Registry::kFrameRate,
				.lane = RouteLane(RouteMarkLane::kMarker), .token = std::to_string(i) });
		}
		edge.marks.push_back({ .atEnd = true, .lane = RouteLane(RouteMarkLane::kReached),
			.token = std::to_string(a_generation) });
		plan.stages.push_back(std::move(edge));

		if (a_destination.layer) {
			auto hold = BuildLayerStage(*a_destination.layer);
			hold.hold = a_destination.layer->holdAt >= 0.0f ? a_destination.layer->holdAt : 1.0f;
			plan.stages.push_back(std::move(hold));
		}
		return plan;
	}

	Animation::ScenePlan BuildRouteTransitionPreviewPlan(
		const Registry::RouteDef& a_route, const Registry::RouteTransition& a_transition)
	{
		auto plan = BasePlan(a_route.id + ":debug:" + a_transition.id);
		plan.speed = 0.0f;
		auto edge = BuildLayerStage(a_transition.layer);
		if (a_transition.contactPose) edge.contactPose = { *a_transition.contactPose };
		edge.blendIn = 0.0f;
		plan.stages.push_back(std::move(edge));
		return plan;
	}
}
