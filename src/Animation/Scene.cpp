#include "Scene.h"

#include <algorithm>

namespace OSF::Animation
{
	Scene::Tick Scene::Advance(const void* a_token, float a_deltaTime)
	{
		std::scoped_lock l{ lock };

		if (!ended.load(std::memory_order_relaxed) && clock.ShouldAdvance(a_token)) {
			const float step = a_deltaTime * speed.load(std::memory_order_relaxed);
			clock.time += step;
			stageElapsed += step;

			bool stageChanged = false;
			const bool wrapped = looped && duration > 0.0f && clock.time >= duration;
			if (!stages.empty()) {
				const auto& stage = stages[currentStage];
				const bool timerExpired = stage.timer > 0.0f && stageElapsed >= stage.timer;
				const bool loopsExpired = stage.loops > 0 && wrapped && (stageLoops + 1) >= stage.loops;
				if (timerExpired || loopsExpired) {
					const char* why = timerExpired ? "timer" : "loop target";
					if (currentStage + 1 < stages.size()) {
						ApplyStageLocked(currentStage + 1);
						REX::INFO("Scene: stage {} expired — advanced to stage {}/{}", why, currentStage + 1, stages.size());
					} else if (loopWhole) {
						ApplyStageLocked(0);  // PlaySequence whole-loop
						REX::INFO("Scene: final stage {} expired — looping to stage 0", why);
					} else {
						ended.store(true, std::memory_order_relaxed);  // hook defers StopScene
						REX::INFO("Scene: final stage {} expired — holding pose, requesting stop", why);
					}
					stageChanged = true;
				}
			}

			// Count the loop + wrap the clock. Skipped on a stage switch (that
			// wrap belongs to the stage we just left).
			if (!stageChanged) {
				if (wrapped) {
					stageLoops++;
				}
				if (duration > 0.0f) {
					if (looped) {
						clock.time = std::fmod(clock.time, duration);
						if (clock.time < 0.0f) {
							clock.time += duration;
						}
					} else {
						clock.time = std::clamp(clock.time, 0.0f, duration);
					}
				}
			}
		}

		return { clock.time, currentStage };
	}

	bool Scene::SetStage(int32_t a_stage)
	{
		std::scoped_lock l{ lock };
		if (a_stage < 0 || static_cast<size_t>(a_stage) >= stages.size()) {
			return false;
		}
		ApplyStageLocked(static_cast<uint32_t>(a_stage));
		ended.store(false, std::memory_order_relaxed);  // a manual jump revives an ended scene
		endQueued.store(false, std::memory_order_relaxed);
		return true;
	}

	void Scene::ApplyStageLocked(uint32_t a_stage)
	{
		currentStage = a_stage;
		stageElapsed = 0.0f;
		stageLoops = 0;
		clock.time = 0.0f;

		const auto& stage = stages[a_stage];
		duration = stage.duration;

		// Element-wise: the pin reads `placements` lock-free, so never reallocate.
		const size_t n = std::min(placements.size(), stage.placements.size());
		for (size_t i = 0; i < n; i++) {
			placements[i] = stage.placements[i];
		}
	}
}
