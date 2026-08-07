#pragma once

#include "Registry/SceneRegistry.h"

#include <cstdint>
#include <optional>

namespace OSF::Overlay
{
	enum class RouteMarkLane : std::uint8_t
	{
		kPropAttach,
		kCommit,
		kPropDestroy,
		kSound,
		kMarker,
		kReached
	};

	[[nodiscard]] constexpr std::uint8_t RouteLane(RouteMarkLane a_lane) noexcept
	{
		return static_cast<std::uint8_t>(a_lane);
	}

	// A zero-animation station compiles to nullopt; callers retain route state and stop playback.
	std::optional<Animation::PlaybackPlan> BuildRouteStationPlan(
		const Registry::RouteDef& a_route, const Registry::RouteStation& a_station);

	Animation::PlaybackPlan BuildRouteTransitionPlan(const Registry::RouteDef& a_route,
		const Registry::RouteTransition& a_transition, const Registry::RouteStation& a_destination,
		std::uint32_t a_generation);

	// The debugger plays only the authored transition. It owns the clock and never schedules side effects
	// or enters the destination station.
	Animation::PlaybackPlan BuildRouteTransitionPreviewPlan(
		const Registry::RouteDef& a_route, const Registry::RouteTransition& a_transition);
}
