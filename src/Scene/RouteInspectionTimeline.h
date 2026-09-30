#pragma once

#include "Registry/SceneRegistry.h"
#include "Util/StringUtil.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace OSF::Scene
{
	inline bool IsInspectionRouteProp(const Registry::RouteProp& a_prop, float a_durationFrames)
	{
		return a_prop.lifetime != Registry::RouteLifetime::kExternal &&
			(a_durationFrames <= 0.0f || a_prop.frame < a_durationFrames);
	}

	inline std::vector<float> InspectionRoutePropMarks(
		const Registry::RouteTransition& a_transition, float a_durationFrames)
	{
		std::vector<float> marks;
		for (const auto& prop : a_transition.props) {
			if (IsInspectionRouteProp(prop, a_durationFrames)) marks.push_back(prop.frame);
		}
		return marks;
	}

	// Reconstruct the props OSF itself would own after processing a route transition through the
	// requested frame. External props are consumer-owned callbacks and are deliberately excluded;
	// transition-lifetime props are gone once the transition-reached lane runs at clip end.
	inline std::vector<const Registry::RouteProp*> InspectionRoutePropsAt(
		const Registry::RouteTransition& a_transition, float a_frame, bool a_atEnd,
		float a_durationFrames = 0.0f)
	{
		std::unordered_map<std::string, const Registry::RouteProp*> desired;
		std::vector<std::string> order;
		for (const auto& prop : a_transition.props) {
			if (prop.frame > a_frame + 0.001f || !IsInspectionRouteProp(prop, a_durationFrames)) {
				continue;
			}
			const auto key = Util::ToLower(prop.id);
			if (prop.attach) {
				if (std::ranges::find(order, key) == order.end()) order.push_back(key);
				desired[key] = &prop;
			} else {
				desired.erase(key);
			}
		}

		std::vector<const Registry::RouteProp*> result;
		for (const auto& key : order) {
			const auto found = desired.find(key);
			if (found == desired.end()) continue;
			if (a_atEnd && found->second->lifetime == Registry::RouteLifetime::kTransition) continue;
			result.push_back(found->second);
		}
		return result;
	}
}
