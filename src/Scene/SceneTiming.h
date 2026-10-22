#pragma once

#include "Registry/ContentRegistry.h"

#include <cmath>
#include <limits>

namespace OSF::Scene
{
	inline void ApplyTerminalTiming(std::int32_t& loops, float& timer, const Registry::NodeDef& node)
	{
		switch (node.loopMode) {
		case Registry::LoopMode::kTimeline: break;
		case Registry::LoopMode::kCount: loops = node.loopCount; timer = 0; break;
		case Registry::LoopMode::kHold: loops = 0; timer = 0; break;
		}
		if (node.timerSec >= 0) timer = node.timerSec;
	}

	// Node policy bounds the terminal stage. Earlier stages retain their own resolved timing.
	inline void ApplyNodeTiming(Animation::PlaybackPlan& a_plan, const Registry::NodeDef& a_node, float a_loopScale)
	{
		if (a_plan.stages.empty()) return;
		auto& terminal = a_plan.stages.back();
		ApplyTerminalTiming(terminal.loops, terminal.timer, a_node);
		if (!std::isfinite(a_loopScale) || a_loopScale <= 0 || a_loopScale == 1) return;
		for (auto& stage : a_plan.stages) {
			if (stage.loops <= 0) continue; // Never turn a hold/timer-only stage into a counted loop.
			const double scaled = std::round(static_cast<double>(stage.loops) * a_loopScale);
			stage.loops = static_cast<std::int32_t>(std::clamp(scaled, 1.0,
				static_cast<double>(std::numeric_limits<std::int32_t>::max())));
		}
	}
}
