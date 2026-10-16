#pragma once

#include "Animation/Scene.h"

namespace OSF::Scene
{
	struct WorldAnchor
	{
		bool set = false;
		RE::NiPoint3 pos{};
		float heading = 0.0f;
	};

	using ActorBaseline = std::vector<std::pair<RE::Actor*, std::pair<RE::NiPoint3, float>>>;

	inline WorldAnchor ComposeSceneAnchor(const WorldAnchor& base, const Animation::ParticipantPlacement& offset)
	{
		return { base.set, Animation::PlacementToWorld(base.pos, base.heading, offset), base.heading + offset.heading };
	}

	struct SceneSpeed
	{
		float value = 1.0f;
		float prePause = 0.0f;
		bool Set(float a_speed)
		{
			if (!std::isfinite(a_speed)) return false;
			if (a_speed <= 0.0f && value > 0.0f) prePause = value;
			value = std::clamp(a_speed, 0.0f, 100.0f);
			return true;
		}
		float ToggleValue() const { return value > 0.0f ? 0.0f : (prePause > 0.0f ? prePause : 1.0f); }
		void Apply(Animation::PlaybackPlan& a_plan) const { a_plan.speed = value; }
	};

	// Identity, rather than role position, owns the original transform across role swaps.
	inline void ApplyScenePlacement(Animation::PlaybackPlan& a_plan, const WorldAnchor& a_explicit,
		const WorldAnchor& a_frozen, const ActorBaseline& a_baseline, const std::vector<RE::Actor*>& a_cast)
	{
		const auto& anchor = a_explicit.set ? a_explicit : a_frozen;
		a_plan.anchorResolved = anchor.set;
		a_plan.restoreTransformsOnEnd = a_explicit.set;
		if (anchor.set) {
			a_plan.anchorPos = anchor.pos;
			a_plan.anchorHeading = anchor.heading;
		}
		a_plan.baselineTransforms.clear();
		for (auto* actor : a_cast) {
			const auto it = std::ranges::find(a_baseline, actor, &ActorBaseline::value_type::first);
			if (it == a_baseline.end()) {
				a_plan.baselineTransforms.clear();
				return;
			}
			a_plan.baselineTransforms.push_back(it->second);
		}
	}
}
