#include "Scene.h"

#include <algorithm>
#include <chrono>

namespace OSF::Animation
{
	Scene::Tick Scene::Advance(const void* a_token, float a_deltaTime)
	{
		std::scoped_lock l{ lock };

		// Liveness heartbeat for the stall watchdog: stamp on EVERY tick (any participant, not just the
		// clock owner — the Scene clock never re-elects, so an owner that stops while others tick must
		// still read as alive). When the engine stops ticking the whole scene, this stops updating.
		lastAdvanceMs.store(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
				.count(),
			std::memory_order_relaxed);

		if (!ended.load(std::memory_order_relaxed) && clock.ShouldAdvance(a_token)) {
			const float step = a_deltaTime * speed.load(std::memory_order_relaxed);
			const float prevTime = clock.time;
			clock.time += step;
			stageElapsed += step;

			bool stageChanged = false;
			const bool wrapped = duration > 0.0f && clock.time >= duration;

			// Fire timed marks for the current stage BEFORE any terminal transition (so an "end" mark and an end edge never race).
			// A numeric mark fires when the playhead crosses its time over [prevTime, clock.time) - handling a single wrap; 
			// an "end" mark fires on the first loop's wrap. repeat:loop fires every loop, else first loop only (gated by markFired[i]). 
			// Fired marks land in firedMarks (drained by the hook); the Scene round-trips the opaque lane+token without interpreting them.
			if (!stages.empty()) {
				const auto& marks = stages[currentStage].marks;
				for (size_t i = 0; i < marks.size(); i++) {
					const auto& mark = marks[i];
					bool crossed = false;
					if (mark.atEnd) {
						crossed = wrapped && stageLoops == 0;
					} else if (duration > 0.0f) {
						const float markTime = mark.fraction * duration;
						crossed = !wrapped ? (prevTime <= markTime && markTime < clock.time)
						                   : (markTime >= prevTime || markTime < (clock.time - duration));
					}
					if (!crossed) {
						continue;
					}
					if (mark.everyLoop) {
						firedMarks.push_back({ mark.lane, mark.token });  // every loop
					} else if (stageLoops == 0 && i < markFired.size() && !markFired[i]) {
						firedMarks.push_back({ mark.lane, mark.token });  // first loop only, once
						markFired[i] = true;
					}
				}
			}

			if (!stages.empty()) {
				const auto& stage = stages[currentStage];
				const bool timerExpired = stage.timer > 0.0f && stageElapsed >= stage.timer;
				const bool loopsExpired = stage.loops > 0 && wrapped && (stageLoops + 1) >= stage.loops;
				if (timerExpired || loopsExpired) {
					const char* why = timerExpired ? "timer" : "loop target";
					if (currentStage + 1 < stages.size()) {
						ApplyStageLocked(currentStage + 1);
						REX::DEBUG("[Anim] stage {} expired — advanced to stage {}/{}", why, currentStage + 1, stages.size());
					} else if (loopWhole) {
						ApplyStageLocked(0);  // PlaySequence whole-loop
						REX::DEBUG("[Anim] final stage {} expired — looping to stage 0", why);
					} else {
						// Record which condition fired so the scene runtime's auto-advance handler can pick the matching auto-edge (timer vs loops/end). 
						// Set before `ended` so a reader gated on `ended` sees the reason.
						endReason.store(timerExpired ? SceneEndReason::kTimer : SceneEndReason::kLoops,
							std::memory_order_relaxed);
						ended.store(true, std::memory_order_relaxed);  // hook defers StopScene
						REX::DEBUG("[Anim] final stage {} expired — holding pose, requesting stop", why);
					}
					stageChanged = true;
				}
			}

			// Count the loop + wrap the clock. Skipped on a stage switch (that wrap belongs to the stage we just left).
			if (!stageChanged) {
				if (wrapped) {
					stageLoops++;
				}
				if (duration > 0.0f) {
					clock.time = std::fmod(clock.time, duration);
					if (clock.time < 0.0f) {
						clock.time += duration;
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

	uint32_t Scene::CurrentStage()
	{
		std::scoped_lock l{ lock };
		return currentStage;
	}

	void Scene::DrainFiredMarks(std::vector<FiredMark>& a_out)
	{
		std::scoped_lock l{ lock };
		a_out.swap(firedMarks);
		firedMarks.clear();
	}

	void Scene::ApplyStageLocked(uint32_t a_stage)
	{
		currentStage = a_stage;
		stageElapsed = 0.0f;
		stageLoops = 0;
		clock.time = 0.0f;

		const auto& stage = stages[a_stage];
		duration = stage.duration;

		// Reset per-pass gating for this stage's marks (all unfired).
		markFired.assign(stage.marks.size(), false);

		// Element-wise: the pin reads `placements` lock-free, so never reallocate.
		const size_t n = std::min(placements.size(), stage.placements.size());
		for (size_t i = 0; i < n; i++) {
			placements[i] = stage.placements[i];
		}
	}
}
