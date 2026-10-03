#pragma once

#include "Registry/SceneRegistry.h"

#include <algorithm>
#include <vector>

namespace OSF::Scene
{
	inline bool IsInspectionProp(const Registry::ActionEntry& a_action)
	{
		return a_action.kind == Registry::ActionKind::kPropAttach ||
			a_action.kind == Registry::ActionKind::kPropDestroy;
	}

	inline std::vector<float> InspectionPropMarks(
		const std::vector<Registry::ActionEntry>& a_actions, float a_durationSec)
	{
		std::vector<float> marks;
		for (const auto& action : a_actions) {
			if (IsInspectionProp(action) && action.pos == Registry::ActionPos::kFraction &&
				Registry::TrackFires(action, a_durationSec)) {
				marks.push_back(Registry::TrackFraction(action, a_durationSec));
			}
		}
		return marks;
	}

	// Applies enter actions, then timed marks up to a_fraction, then end actions if a_atEnd.
	// a_durationSec places `atFrame` actions on the fraction axis (0 = unknown).
	inline std::vector<Registry::ActionEntry> InspectionPropsAt(
		const std::vector<Registry::ActionEntry>& a_actions, float a_fraction, bool a_atEnd,
		float a_durationSec = 0.0f)
	{
		std::vector<Registry::ActionEntry> desired;
		const auto apply = [&](const Registry::ActionEntry& a_action) {
			if (!IsInspectionProp(a_action)) {
				return;
			}
			const auto existing = std::find_if(desired.begin(), desired.end(),
				[&](const Registry::ActionEntry& a_live) { return a_live.prop == a_action.prop; });
			if (a_action.kind == Registry::ActionKind::kPropDestroy) {
				if (existing != desired.end()) {
					desired.erase(existing);
				}
			} else if (existing != desired.end()) {
				*existing = a_action;
			} else {
				desired.push_back(a_action);
			}
		};

		for (const auto& action : a_actions) {
			if (action.pos == Registry::ActionPos::kEnter) {
				apply(action);
			}
		}
		const float fraction = std::clamp(a_fraction, 0.0f, 1.0f);
		std::vector<const Registry::ActionEntry*> timed;
		for (const auto& action : a_actions) {
			// The runtime never fires an `atFrame` past the clip end; match it.
			if (IsInspectionProp(action) && action.pos == Registry::ActionPos::kFraction &&
				Registry::TrackFires(action, a_durationSec) &&
				Registry::TrackFraction(action, a_durationSec) <= fraction) {
				timed.push_back(&action);
			}
		}
		std::stable_sort(timed.begin(), timed.end(), [&](const auto* a_left, const auto* a_right) {
			return Registry::TrackFraction(*a_left, a_durationSec) <
			       Registry::TrackFraction(*a_right, a_durationSec);
		});
		for (const auto* action : timed) {
			apply(*action);
		}
		if (a_atEnd) {
			for (const auto& action : a_actions) {
				if (action.pos == Registry::ActionPos::kEnd) {
					apply(action);
				}
			}
		}
		return desired;
	}
}
