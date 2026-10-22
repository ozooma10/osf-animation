#include "Scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace OSF::Animation
{
	std::optional<std::vector<PlaybackSession::SegmentData>> PreparePlaybackStages(
		const PlaybackPlan& plan, std::size_t actorCount, const PlaybackClipLoader& load)
	{
		if (!actorCount || plan.stages.empty() || !load) return std::nullopt;
		std::vector<PlaybackSession::SegmentData> stages;
		stages.reserve(plan.stages.size());
		// The resource reader can do IO before the import cache is consulted. Share exact
		// clip requests within this preparation only; a later attempt loads fresh normally.
		std::map<std::pair<std::string, std::string>, PlaybackSession::ParticipantSlot> clips;
		for (const auto& source : plan.stages) {
			if (source.files.size() != actorCount ||
				(!source.animIds.empty() && source.animIds.size() != actorCount)) return std::nullopt;
			PlaybackSession::SegmentData stage;
			stage.timer = source.timer;
			stage.loops = source.loops;
			stage.hold = std::isfinite(source.hold) && source.hold >= 0.0f ? std::min(source.hold, 1.0f) : -1.0f;
			const float defaultBlend = std::isfinite(plan.blendIn) && plan.blendIn >= 0.0f ? plan.blendIn : 0.4f;
			stage.blendIn = std::isfinite(source.blendIn) && source.blendIn >= 0.0f ? source.blendIn : defaultBlend;
			stage.placements = source.placements.empty() ? std::vector<ParticipantPlacement>(actorCount) : source.placements;
			stage.masks = source.masks.empty() ? (plan.masks.empty() ? std::vector<std::string>(actorCount) : plan.masks) : source.masks;
			stage.poseModes = source.poseModes.empty() ? (plan.poseModes.empty() ? std::vector<PoseMode>(actorCount, PoseMode::kOverride) : plan.poseModes) : source.poseModes;
			stage.poseWeights = source.poseWeights.empty() ? (plan.poseWeights.empty() ? std::vector<float>(actorCount, 1.0f) : plan.poseWeights) : source.poseWeights;
			stage.marks = source.marks;
			stage.participants.reserve(actorCount);
			for (std::size_t i = 0; i < actorCount; ++i) {
				const AnimationClipSpec spec{ source.files[i], source.animIds.empty() ? std::string{} : source.animIds[i] };
				const auto key = std::pair{ spec.resourcePath, spec.animationId };
				auto found = clips.find(key);
				if (found == clips.end()) {
					auto clip = load(spec);
					if (!clip || !clip->skeleton || !clip->skeleton->data || !clip->anim || !clip->anim->data)
						return std::nullopt;
					const float duration = clip->anim->data->duration();
					if (!std::isfinite(duration) || duration <= 0.0f) return std::nullopt;
					clip->file = spec.resourcePath;
					found = clips.emplace(key, std::move(*clip)).first;
				}
				stage.participants.push_back(found->second);
			}
			stage.duration = stage.participants.front().anim->data->duration();
			for (std::size_t i = 1; i < stage.participants.size(); ++i) {
				const float duration = stage.participants[i].anim->data->duration();
				if (std::abs(duration - stage.duration) > 0.01f)
					REX::WARN("[Anim] synchronized playback: segment participant {} duration {:.3f}s differs from reference {:.3f}s; sampling wraps independently",
						i, duration, stage.duration);
			}
			stages.push_back(std::move(stage));
		}
		return stages;
	}

	PlaybackSession::Tick PlaybackSession::Advance(const void* a_token, float a_deltaTime)
	{
		return Advance(a_token, a_deltaTime, std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	PlaybackSession::Tick PlaybackSession::Advance(const void* a_token, float a_deltaTime, std::int64_t a_nowMs)
	{
		std::scoped_lock l{ lock };

		// Any participant is a heartbeat; FrameClock re-elects a stale owner.
		lastAdvanceMs.store(a_nowMs, std::memory_order_relaxed);

		if (!preparationHeld.load(std::memory_order_relaxed) && pendingSegment.load(std::memory_order_relaxed) < 0 &&
            !ended.load(std::memory_order_relaxed) && clock.ShouldAdvance(a_token, a_nowMs)) {
			float remaining = a_deltaTime * speed.load(std::memory_order_relaxed);
			if (!std::isfinite(remaining) || remaining <= 0.0f) {
				remaining = 0.0f;
			}
#ifdef OSF_ANIMATION_SMOKE
			const float d3SecondsPerMotion = remaining > 0.0f ? a_deltaTime / remaining : 0.0f;
#endif

			// One loop/timer boundary at a time keeps loop counts and repeat marks exact at high speed.
			while (remaining > 0.0f && !ended.load(std::memory_order_relaxed) && !stages.empty()) {
				const auto& stage = stages[currentSegment];
				if (stage.hold >= 0.0f) {
					// Freeze frame: only `timer` can expire it. Marks at/before the hold pose fire once here
					// (so `action at:0` still runs).
					const float segment = autoAdvance && stage.timer > 0.0f ?
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
					if (!(autoAdvance && stage.timer > 0.0f && stageElapsed >= stage.timer)) {
						break;
					}
					if (currentSegment + 1 < stages.size()) {
						AdvanceSegmentLocked(currentSegment + 1);
						REX::DEBUG("[Anim] held playback segment timer expired — advanced to segment {}/{}", currentSegment + 1, stages.size());
					} else if (loopWhole) {
						AdvanceSegmentLocked(0);
						REX::DEBUG("[Anim] final held playback segment timer expired — looping to segment 0");
					} else {
						endReason.store(PlaybackEndReason::kTimerComplete, std::memory_order_relaxed);
						ended.store(true, std::memory_order_relaxed);
						REX::DEBUG("[Anim] final held playback segment timer expired — holding pose, requesting stop");
					}
					break;
				}
				const float toWrap = duration > 0.0f ? std::max(0.0f, duration - clock.time) : remaining;
				const float toTimer = autoAdvance && stage.timer > 0.0f ? std::max(0.0f, stage.timer - stageElapsed) : remaining;
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
#ifdef OSF_ANIMATION_SMOKE
					const auto before = firedMarks.size();
#endif
					if (mark.everyLoop) {
						firedMarks.push_back({ mark.lane, mark.token });
					} else if (stageLoops == 0 && i < markFired.size() && !markFired[i]) {
						firedMarks.push_back({ mark.lane, mark.token });
						markFired[i] = true;
					}
#ifdef OSF_ANIMATION_SMOKE
					if (firedMarks.size() != before) {
						auto& fired = firedMarks.back();
						fired.d3InputDelta = a_deltaTime;
						fired.d3MotionLateNs = static_cast<std::uint64_t>(std::max(0.0f,
							(remaining - (markTime - prevTime)) * d3SecondsPerMotion) * 1e9);
					}
#endif
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
					// One-shot final segment done: park at the clip end so the cast holds the last pose until the
					// deferred stop lands.
					const bool finishing = autoAdvance && stage.loops > 0 && stageLoops >= stage.loops &&
					                       (prepareStages || (currentSegment + 1 >= stages.size() && !loopWhole));
					clock.time = finishing ? duration : 0.0f;
				}

				const bool timerExpired = autoAdvance && stage.timer > 0.0f && stageElapsed >= stage.timer;
				const bool loopsExpired = autoAdvance && stage.loops > 0 && wrapped && stageLoops >= stage.loops;
				if (!timerExpired && !loopsExpired) {
					continue;
				}

				const char* why = timerExpired ? "timer" : "loop target";
				if (currentSegment + 1 < stages.size()) {
					AdvanceSegmentLocked(currentSegment + 1);
					REX::DEBUG("[Anim] playback segment {} expired — advanced to segment {}/{}", why, currentSegment + 1, stages.size());
				} else if (loopWhole) {
					AdvanceSegmentLocked(0);
					REX::DEBUG("[Anim] final playback segment {} expired — looping to segment 0", why);
				} else {
					endReason.store(timerExpired ? PlaybackEndReason::kTimerComplete : PlaybackEndReason::kLoopComplete,
						std::memory_order_relaxed);
					ended.store(true, std::memory_order_relaxed);
					REX::DEBUG("[Anim] final playback segment {} expired — holding pose, requesting stop", why);
				}
				// A segment transition consumes the rest of this update.
				remaining = 0.0f;
			}
		}

		// Mid-clip preparation freezes time without changing a shorter participant's wrapping.
		// Only an actual loop-end boundary clamps to the final pose.
		const bool holdEnd = (pendingSegment.load(std::memory_order_relaxed) >= 0 && duration > 0 && clock.time >= duration) || (ended.load(std::memory_order_relaxed) && endReason.load(std::memory_order_relaxed) == PlaybackEndReason::kLoopComplete);
		const bool holding = currentSegment < stages.size() && stages[currentSegment].hold >= 0.0f;
		return { clock.time, currentSegment, holdEnd, holding };
	}

	void PlaybackSession::AdvanceSegmentLocked(uint32_t segment)
    {
        if (prepareStages) pendingSegment.store(static_cast<int32_t>(segment), std::memory_order_relaxed);
        else ApplySegmentLocked(segment);
    }

	bool PlaybackSession::HoldForPreparation(bool hold)
	{
		std::scoped_lock l{ lock };
		if (!hold && (ended.load(std::memory_order_relaxed) || pendingSegment.load(std::memory_order_relaxed) >= 0))
			return false;
		preparationHeld.store(hold, std::memory_order_relaxed);
		return true;
	}

	bool PlaybackSession::RequestSegment(int32_t segment)
    {
        std::scoped_lock l{lock};
        if (!prepareStages || segment < 0 || static_cast<size_t>(segment) >= stages.size() ||
            pendingSegment.load(std::memory_order_relaxed) >= 0) return false;
        pendingSegment.store(segment, std::memory_order_relaxed);
        stageQueued.store(false, std::memory_order_relaxed);
        return true;
    }

    bool PlaybackSession::SetSegment(int32_t a_segment, bool a_prepared)
	{
		std::scoped_lock l{ lock };
		if ((prepareStages && !a_prepared) || a_segment < 0 || static_cast<size_t>(a_segment) >= stages.size()) {
			return false;
		}
		ApplySegmentLocked(static_cast<uint32_t>(a_segment));
        pendingSegment.store(-1, std::memory_order_relaxed);
        stageQueued.store(false, std::memory_order_relaxed);
		ended.store(false, std::memory_order_relaxed);  // a manual jump revives an ended playback session
		endQueued.store(false, std::memory_order_relaxed);
		return true;
	}

	bool PlaybackSession::SetAutoAdvance(bool a_enabled)
	{
		std::scoped_lock l{ lock };
		if (ended.load(std::memory_order_relaxed)) return false;
		if (autoAdvance != a_enabled && a_enabled) {
			stageElapsed = 0.0f;
			stageLoops = 0;
		}
		autoAdvance = a_enabled;
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
		// a_out is replaced, never appended. Swapping hands its (cleared) capacity back to firedMarks.
		a_out.clear();
		if (!firedMarks.empty()) {
			a_out.swap(firedMarks);
		}
	}

	void PlaybackSession::ApplySegmentLocked(uint32_t a_segment)
	{
		currentSegment = a_segment;
		stageElapsed = 0.0f;
		stageLoops = 0;
		clock.time = 0.0f;

		const auto& stage = stages[a_segment];
		duration = stage.duration;

		// Frozen segment shows its hold pose from the first sample; a ratio of exactly 1 wraps to frame 0.
		holdMarksFired = false;
		if (stage.hold >= 0.0f && duration > 0.0f) {
			clock.time = std::clamp(stage.hold * duration, 0.0f, std::nextafter(duration, 0.0f));
		}

		markFired.assign(stage.marks.size(), false);

	}
}
