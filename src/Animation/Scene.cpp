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
					if (currentStage + 1 < stages.size()) {
						ApplyStageLocked(currentStage + 1);
						stageChanged = true;
						REX::INFO("Scene: stage {} expired — auto-advanced to stage {}/{}",
							timerExpired ? "timer" : "loop target", currentStage + 1, stages.size());
					} else if (loopWhole) {
						// Whole-sequence loop (PlaySequence): restart at stage 0
						// instead of ending.
						ApplyStageLocked(0);
						stageChanged = true;
						REX::INFO("Scene: final stage {} expired — looping whole sequence to stage 0",
							timerExpired ? "timer" : "loop target");
					} else {
						// Last timed/loop-counted stage ran out: hold the pose;
						// the update hook defers the actual StopScene to the
						// game thread.
						ended.store(true, std::memory_order_relaxed);
						stageChanged = true;
						REX::INFO("Scene: final stage {} expired — holding pose, requesting stop",
							timerExpired ? "timer" : "loop target");
					}
				}
			}

			// Count loops, then wrap the clock. Skipped on the step that
			// switched/ended the stage: that wrap belongs to the stage we're
			// leaving.
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
		// a manual jump revives an ended (but not yet stopped) scene
		ended.store(false, std::memory_order_relaxed);
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

		// element-wise: the compose-root pin reads `placements` without the
		// scene lock, so the buffer must never reallocate
		const size_t n = std::min(placements.size(), stage.placements.size());
		for (size_t i = 0; i < n; i++) {
			placements[i] = stage.placements[i];
		}
	}
}
