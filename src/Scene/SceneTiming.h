#pragma once

#include "Registry/ContentRegistry.h"

#include <cmath>
#include <limits>

namespace OSF::Scene
{
	// Node policy bounds the terminal stage. Earlier stages retain their own resolved timing.
	inline void ApplyNodeTiming(Animation::PlaybackPlan& a_plan, const Registry::SceneNode& a_node, float a_loopScale)
	{
		if (a_plan.stages.empty()) return;
		auto& terminal = a_plan.stages.back();
		switch (a_node.loopMode) {
		case Registry::LoopMode::kOnce: terminal.loops = 1; break;
		case Registry::LoopMode::kCount: terminal.loops = a_node.loopCount; break;
		case Registry::LoopMode::kHold: terminal.loops = 0; break;
		}
		const bool hasTimerEdge = std::ranges::any_of(a_node.edges, [](const auto& edge) {
			return edge.when == Registry::EdgeWhen::kTimer;
		});
		terminal.timer = hasTimerEdge && a_node.timerSec > 0 ? a_node.timerSec : 0;
		if (!std::isfinite(a_loopScale) || a_loopScale <= 0 || a_loopScale == 1) return;
		for (auto& stage : a_plan.stages) {
			if (stage.loops <= 0) continue; // Never turn a hold/timer-only stage into a counted loop.
			const double scaled = std::round(static_cast<double>(stage.loops) * a_loopScale);
			stage.loops = static_cast<std::int32_t>(std::clamp(scaled, 1.0,
				static_cast<double>(std::numeric_limits<std::int32_t>::max())));
		}
	}
}
