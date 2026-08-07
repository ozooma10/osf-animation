#pragma once

#include "Registry/SceneRegistry.h"
#include "Util/StringUtil.h"

#include <unordered_map>
#include <vector>

namespace OSF::Scene
{
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
			if (prop.frame > a_frame + 0.001f ||
				(a_durationFrames > 0.0f && prop.frame >= a_durationFrames) ||
				prop.lifetime == Registry::RouteLifetime::kExternal) {
				continue;
			}
			const auto key = Util::ToLower(prop.id);
			if (prop.attach) {
				if (!desired.contains(key)) order.push_back(key);
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
