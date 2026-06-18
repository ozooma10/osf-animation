#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace OSF::Animation
{
	// Advances a clock from a stream of (token, dt) update reports without a global frame counter.
	// Engine calls AnimationManager::Update several times per render frame per graph with subdivided dt
	// Can be multiple managers so be careful of summing dt, which can cause over-advances.
	// The first manager reported becomes owner of FrameClock, and only it is responsible for advancing time
	// Other managers just read clock (and if owner stops updating, a scene clock stalls [sync group owner is re-elected])
	struct FrameClock
	{
		const void* owner = nullptr;
		float time = 0.0f;

		// True if this report's dt should be added to `time`
		// (token is owning AnimationManager)
		bool ShouldAdvance(const void* a_token)
		{
			if (!owner) {
				owner = a_token;
			}
			return a_token == owner;
		}

		void Reset()
		{
			owner = nullptr;
			time = 0.0f;
		}
	};

	// Timeout before electing new owner of syncgroup to drive frameclock. (next ShouldAdvance caller becomes owner)
	// (owner silence is a sign the owner stopped updating, e.g. actor unloaded or playback stopped without cleanup).
	inline constexpr int64_t kSyncOwnerStaleMs = 250;

	// SyncGroup is effectively a shared clock for actors in sync group.
	// clock *does not* wrap; each graph wraps it by its own clip duration, 
	// so mismatched-length clips still loop independently while sharing a phase origin.
	struct SyncGroup
	{
		std::mutex lock;
		FrameClock clock;
		
		// track last time owner advanced for stale detection
		int64_t lastAdvanceMs = 0;
		
		// Playback speed for all members. Any member's SetSpeed sets it and the clock advances by it regardless of which graph owns the clock
		std::atomic<float> speed{ 1.0f };
	};
}
