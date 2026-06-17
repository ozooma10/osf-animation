#include "Scene.h"

#include <algorithm>

namespace OSF::Animation
{
	Scene::Tick Scene::Advance(const void* a_token, float a_deltaTime)
	{
		std::scoped_lock l{ lock };

		if (!ended.load(std::memory_order_relaxed) && clock.ShouldAdvance(a_token)) {
			const float step = a_deltaTime * speed.load(std::memory_order_relaxed);
			const float prevTime = clock.time;
			clock.time += step;
			stageElapsed += step;

			bool stageChanged = false;
			const bool wrapped = looped && duration > 0.0f && clock.time >= duration;

			// Fire timed cues for the current stage BEFORE any terminal transition (so an
			// "end" cue and an end edge never race). A numeric mark fires when the playhead
			// crosses its time over [prevTime, clock.time) — handling a single wrap; an "end"
			// mark fires on the first loop's wrap. repeat:loop fires every loop, else first
			// loop only (gated by cueFired[i]). Fired ids land in firedCues (drained by the hook).
			if (!stages.empty()) {
				const auto& cues = stages[currentStage].cues;
				for (size_t i = 0; i < cues.size(); i++) {
					const auto& cue = cues[i];
					bool crossed = false;
					if (cue.atEnd) {
						crossed = wrapped && stageLoops == 0;
					} else if (duration > 0.0f) {
						const float markTime = cue.fraction * duration;
						crossed = !wrapped ? (prevTime <= markTime && markTime < clock.time)
						                   : (markTime >= prevTime || markTime < (clock.time - duration));
					}
					if (!crossed) {
						continue;
					}
					if (cue.everyLoop) {
						firedCues.push_back(cue.id);  // every loop
					} else if (stageLoops == 0 && i < cueFired.size() && !cueFired[i]) {
						firedCues.push_back(cue.id);  // first loop only, once
						cueFired[i] = true;
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
						REX::INFO("Scene: stage {} expired — advanced to stage {}/{}", why, currentStage + 1, stages.size());
					} else if (loopWhole) {
						ApplyStageLocked(0);  // PlaySequence whole-loop
						REX::INFO("Scene: final stage {} expired — looping to stage 0", why);
					} else {
						// Record which condition fired so the Layer-B auto-advance handler
						// can pick the matching auto-edge (timer vs loops/end). Set BEFORE
						// `ended` so a reader gated on `ended` sees the reason.
						endReason.store(timerExpired ? SceneEndReason::kTimer : SceneEndReason::kLoops,
							std::memory_order_relaxed);
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

	uint32_t Scene::CurrentStage()
	{
		std::scoped_lock l{ lock };
		return currentStage;
	}

	void Scene::DrainFiredCues(std::vector<std::string>& a_out)
	{
		std::scoped_lock l{ lock };
		a_out.swap(firedCues);
		firedCues.clear();
	}

	void Scene::ApplyStageLocked(uint32_t a_stage)
	{
		currentStage = a_stage;
		stageElapsed = 0.0f;
		stageLoops = 0;
		clock.time = 0.0f;

		const auto& stage = stages[a_stage];
		duration = stage.duration;

		// Reset per-pass cue gating for this stage's marks (all unfired).
		cueFired.assign(stage.cues.size(), false);

		// Element-wise: the pin reads `placements` lock-free, so never reallocate.
		const size_t n = std::min(placements.size(), stage.placements.size());
		for (size_t i = 0; i < n; i++) {
			placements[i] = stage.placements[i];
		}
	}
}
