#include "Scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace OSF::Animation
{
	PlaybackSession::Tick PlaybackSession::Advance(const void* a_token, float a_deltaTime)
	{
		std::scoped_lock l{ lock };

		const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		// Any participant is a liveness heartbeat. FrameClock independently re-elects a stale owner,
		// so a surviving participant both keeps the playback session alive and resumes its clock.
		lastAdvanceMs.store(nowMs, std::memory_order_relaxed);

		if (!ended.load(std::memory_order_relaxed) && clock.ShouldAdvance(a_token, nowMs)) {
			float remaining = a_deltaTime * speed.load(std::memory_order_relaxed);
			if (!std::isfinite(remaining) || remaining <= 0.0f) {
				return { clock.time, currentSegment };
			}

			// Consume the update one loop/timer boundary at a time. This keeps loop counts and
			// repeat:loop marks exact even when a high playback speed crosses several loops at once.
			while (remaining > 0.0f && !ended.load(std::memory_order_relaxed) && !stages.empty()) {
				const auto& stage = stages[currentSegment];
				if (stage.hold >= 0.0f) {
					// Freeze-frame segment: the clip does not advance, so no wrap and no loop count can
					// ever expire. Only `timer` (or a manual advance/stop) leaves it. The clock is
					// already parked on the hold pose by ApplySegmentLocked; the marks that sit at or
					// before that pose fire once, here, so an authored `action at:0` still runs.
					const float segment = stage.timer > 0.0f ?
					                          std::min(remaining, std::max(0.0f, stage.timer - stageElapsed)) :
					                          remaining;
					if (!holdMarksFired) {
						holdMarksFired = true;
						for (size_t i = 0; i < stage.marks.size(); i++) {
							const auto& mark = stage.marks[i];
							const bool reached = mark.atEnd ? stage.hold >= 1.0f :
							                                  duration > 0.0f && MarkTime(mark, duration) <= clock.time;
							if (!reached || i >= markFired.size() || markFired[i]) {
								continue;
							}
							firedMarks.push_back({ mark.lane, mark.token });
							markFired[i] = true;
						}
					}
					stageElapsed += std::max(segment, 0.0f);
					remaining = 0.0f;
					if (!(stage.timer > 0.0f && stageElapsed >= stage.timer)) {
						break;  // still holding
					}
					if (currentSegment + 1 < stages.size()) {
						ApplySegmentLocked(currentSegment + 1);
						REX::DEBUG("[Anim] held playback segment timer expired — advanced to segment {}/{}", currentSegment + 1, stages.size());
					} else if (loopWhole) {
						ApplySegmentLocked(0);
						REX::DEBUG("[Anim] final held playback segment timer expired — looping to segment 0");
					} else {
						endReason.store(PlaybackEndReason::kTimerComplete, std::memory_order_relaxed);
						ended.store(true, std::memory_order_relaxed);
						REX::DEBUG("[Anim] final held playback segment timer expired — holding pose, requesting stop");
					}
					break;
				}
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
					const float markTime = MarkTime(mark, duration);
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
					// A one-shot final playback segment that just consumed its last loop ENDS here: hold the
					// clock at the clip end so participants keep the final pose while the deferred
					// stop lands. Resetting to 0 (the looping path) snapped the cast back to the
					// clip's first frame for however many frames the stop task took to arrive.
					const bool finishing = stage.loops > 0 && stageLoops >= stage.loops &&
					                       currentSegment + 1 >= stages.size() && !loopWhole;
					clock.time = finishing ? duration : 0.0f;
				}

				const bool timerExpired = stage.timer > 0.0f && stageElapsed >= stage.timer;
				const bool loopsExpired = stage.loops > 0 && wrapped && stageLoops >= stage.loops;
				if (!timerExpired && !loopsExpired) {
					continue;
				}

				const char* why = timerExpired ? "timer" : "loop target";
				if (currentSegment + 1 < stages.size()) {
					ApplySegmentLocked(currentSegment + 1);
					REX::DEBUG("[Anim] playback segment {} expired — advanced to segment {}/{}", why, currentSegment + 1, stages.size());
				} else if (loopWhole) {
					ApplySegmentLocked(0);
					REX::DEBUG("[Anim] final playback segment {} expired — looping to segment 0", why);
				} else {
					endReason.store(timerExpired ? PlaybackEndReason::kTimerComplete : PlaybackEndReason::kLoopComplete,
						std::memory_order_relaxed);
					ended.store(true, std::memory_order_relaxed);
					REX::DEBUG("[Anim] final playback segment {} expired — holding pose, requesting stop", why);
				}
				// Preserve the prior behavior: a segment transition consumes the current update; the
				// next engine report begins the new segment rather than skipping through several segments.
				remaining = 0.0f;
			}
		}

		const bool holdEnd = ended.load(std::memory_order_relaxed) &&
		                     endReason.load(std::memory_order_relaxed) == PlaybackEndReason::kLoopComplete;
		return { clock.time, currentSegment, holdEnd };
	}

	bool PlaybackSession::SetSegment(int32_t a_segment)
	{
		std::scoped_lock l{ lock };
		if (a_segment < 0 || static_cast<size_t>(a_segment) >= stages.size()) {
			return false;
		}
		ApplySegmentLocked(static_cast<uint32_t>(a_segment));
		ended.store(false, std::memory_order_relaxed);  // a manual jump revives an ended playback session
		endQueued.store(false, std::memory_order_relaxed);
		return true;
	}

	bool PlaybackSession::Seek(float a_time)
	{
		if (!std::isfinite(a_time)) {
			return false;
		}
		std::scoped_lock l{ lock };
		if (stages.empty()) {
			return false;
		}

		// SamplingJob accepts the end ratio, but the normal playback-session sampling path wraps a live clip.
		// Keep a scrub at 100% on the last representable pose instead of snapping to frame zero.
		const float lastPose = duration > 0.0f ? std::nextafter(duration, 0.0f) : 0.0f;
		clock.time = std::clamp(a_time, 0.0f, lastPose);
		// On a playing segment the clip position IS the time spent in it, so a scrub carries any timer
		// with it. A frozen segment's clock is a pose, not elapsed time — moving it must not rewind
		// (or expire) the timer that is the only way out.
		if (stages[currentSegment].hold < 0.0f) {
			stageElapsed = clock.time;
		}
		stageLoops = 0;
		ended.store(false, std::memory_order_relaxed);
		endQueued.store(false, std::memory_order_relaxed);
		firedMarks.clear();

		const auto& marks = stages[currentSegment].marks;
		markFired.assign(marks.size(), false);
		if (duration > 0.0f) {
			for (size_t i = 0; i < marks.size(); i++) {
				const auto& mark = marks[i];
				if (!mark.everyLoop && !mark.atEnd && MarkTime(mark, duration) < clock.time) {
					markFired[i] = true;
				}
			}
		}
		return true;
	}

	PlaybackSession::PlaybackSnapshot PlaybackSession::GetPlaybackSnapshot()
	{
		std::scoped_lock l{ lock };
		return { clock.time, duration, speed.load(std::memory_order_relaxed), currentSegment };
	}

	uint32_t PlaybackSession::CurrentSegment()
	{
		std::scoped_lock l{ lock };
		return currentSegment;
	}

	PlaybackSession::DiagnosticSnapshot PlaybackSession::GetDiagnosticSnapshot(std::int64_t a_nowMs)
	{
		std::scoped_lock l{ lock };
		const std::int64_t ownerAge = clock.owner && clock.lastAdvanceMs > 0 ? a_nowMs - clock.lastAdvanceMs : -1;
		return { clock.time, currentSegment, clock.owner, ownerAge };
	}

	void PlaybackSession::DrainFiredMarks(std::vector<FiredMark>& a_out)
	{
		std::scoped_lock l{ lock };
		a_out.swap(firedMarks);
		firedMarks.clear();
	}

	void PlaybackSession::ApplySegmentLocked(uint32_t a_segment)
	{
		currentSegment = a_segment;
		stageElapsed = 0.0f;
		stageLoops = 0;
		clock.time = 0.0f;

		const auto& stage = stages[a_segment];
		duration = stage.duration;

		// A frozen segment shows its hold pose from the very first sample, before any Advance runs.
		// The last frame is the pose at nextafter(duration, 0) — a ratio of exactly 1 wraps to frame 0.
		holdMarksFired = false;
		if (stage.hold >= 0.0f && duration > 0.0f) {
			clock.time = std::clamp(stage.hold * duration, 0.0f, std::nextafter(duration, 0.0f));
		}

		// Reset per-pass gating for this segment's marks (all unfired).
		markFired.assign(stage.marks.size(), false);

	}
}
