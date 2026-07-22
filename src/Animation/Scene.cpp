#include "Scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace OSF::Animation
{
	Scene::Tick Scene::Advance(const void* a_token, float a_deltaTime)
	{
		std::scoped_lock l{ lock };

		const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		// Any participant is a liveness heartbeat. FrameClock independently re-elects a stale owner,
		// so a surviving participant both keeps the scene alive and resumes its clock.
		lastAdvanceMs.store(nowMs, std::memory_order_relaxed);

		if (!ended.load(std::memory_order_relaxed) && clock.ShouldAdvance(a_token, nowMs)) {
			float remaining = a_deltaTime * speed.load(std::memory_order_relaxed);
			if (!std::isfinite(remaining) || remaining <= 0.0f) {
				return { clock.time, currentStage };
			}

			// Consume the update one loop/timer boundary at a time. This keeps loop counts and
			// repeat:loop marks exact even when a high playback speed crosses several loops at once.
			while (remaining > 0.0f && !ended.load(std::memory_order_relaxed) && !stages.empty()) {
				const auto& stage = stages[currentStage];
				const float toWrap = duration > 0.0f ? std::max(0.0f, duration - clock.time) : remaining;
				const float toTimer = stage.timer > 0.0f ? std::max(0.0f, stage.timer - stageElapsed) : remaining;
				float segment = std::min({ remaining, toWrap, toTimer });
				// Floating-point equality at a boundary can otherwise leave a zero-length loop.
				if (!(segment > 0.0f)) {
					segment = std::min(remaining, 0.000001f);
				}

				const float prevTime = clock.time;
				const float nextTime = clock.time + segment;
				for (size_t i = 0; i < stage.marks.size(); i++) {
					const auto& mark = stage.marks[i];
					if (mark.atEnd || duration <= 0.0f) {
						continue;
					}
					const float markTime = mark.fraction * duration;
					if (!(prevTime <= markTime && markTime < nextTime)) {
						continue;
					}
					if (mark.everyLoop) {
						firedMarks.push_back({ mark.lane, mark.token });
					} else if (stageLoops == 0 && i < markFired.size() && !markFired[i]) {
						firedMarks.push_back({ mark.lane, mark.token });
						markFired[i] = true;
					}
				}

				clock.time = nextTime;
				stageElapsed += segment;
				remaining -= segment;

				const bool wrapped = duration > 0.0f && clock.time >= duration;
				if (wrapped) {
					for (size_t i = 0; i < stage.marks.size(); i++) {
						const auto& mark = stage.marks[i];
						if (mark.atEnd && stageLoops == 0 && i < markFired.size() && !markFired[i]) {
							firedMarks.push_back({ mark.lane, mark.token });
							markFired[i] = true;
						}
					}
					stageLoops++;
					clock.time = 0.0f;
				}

				const bool timerExpired = stage.timer > 0.0f && stageElapsed >= stage.timer;
				const bool loopsExpired = stage.loops > 0 && wrapped && stageLoops >= stage.loops;
				if (!timerExpired && !loopsExpired) {
					continue;
				}

				const char* why = timerExpired ? "timer" : "loop target";
				if (currentStage + 1 < stages.size()) {
					ApplyStageLocked(currentStage + 1);
					REX::DEBUG("[Anim] stage {} expired — advanced to stage {}/{}", why, currentStage + 1, stages.size());
				} else if (loopWhole) {
					ApplyStageLocked(0);
					REX::DEBUG("[Anim] final stage {} expired — looping to stage 0", why);
				} else {
					endReason.store(timerExpired ? SceneEndReason::kTimer : SceneEndReason::kLoops,
						std::memory_order_relaxed);
					ended.store(true, std::memory_order_relaxed);
					REX::DEBUG("[Anim] final stage {} expired — holding pose, requesting stop", why);
				}
				// Preserve the prior behavior: a stage transition consumes the current update; the
				// next engine report begins the new stage rather than skipping through several nodes.
				remaining = 0.0f;
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

	}
}
