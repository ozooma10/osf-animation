#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace OSF::Animation
{
	// Advances a clock from a stream of (token, dt) update reports without a
	// global frame counter. The engine calls AnimationManager::Update several
	// times per render frame per graph with a subdivided dt (~2.3 ms slices on
	// 1.16.242, live-observed), so per-call dt sums to wall time for any ONE
	// manager — but summing across managers (player 1st+3rd person, or scene
	// participants) over-advances by up to the manager count, and frame-turnover
	// heuristics based on token repetition are order-dependent under
	// subdivision. Instead: the first manager ever reported becomes the owner,
	// and only its reports advance time. Other managers just read the clock.
	// (If the owner stops updating — e.g. its actor unloads — the clock stalls;
	// GraphManager::WatchdogSweep detects that via Scene::lastOwnerAdvanceMs
	// and ends the stalled scene.)
	struct FrameClock
	{
		const void* owner = nullptr;
		float time = 0.0f;

		// True if this report's dt should be added to `time` (i.e. a_token is
		// the owning manager). True for the very first reporter.
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

	// A shared-clock owner that goes silent longer than this (its graph stopped
	// or its actor unloaded) is re-elected so the group keeps running instead of
	// freezing on the dead owner. Comfortably longer than the worst inter-call
	// gap (the hook fires several times per render frame).
	inline constexpr int64_t kSyncOwnerStaleMs = 250;

	// Shared clock for a Sync group: N already-playing solo graphs put on one
	// owner-token FrameClock so they stay frame-locked. Each graph is guarded by
	// its own Graph::lock, so the shared clock carries its own LEAF lock (taken
	// under a graph lock, never the reverse). The clock free-runs (no wrap); each
	// graph wraps the shared time by its OWN clip duration, so mismatched-length
	// clips still loop independently while sharing a phase origin.
	struct SyncGroup
	{
		std::mutex lock;
		FrameClock clock;
		// steady_clock ms when the owner last advanced the clock; lets a member
		// detect a silent owner and re-elect (Graph::Sample). 0 = never advanced,
		// which only coincides with owner == nullptr (no re-election needed yet).
		int64_t lastAdvanceMs = 0;
		// Group playback speed shared by all members: any member's SetSpeed sets
		// it and the clock advances by it regardless of which graph owns the clock
		// (so speed control is deterministic, not tied to the owner race).
		std::atomic<float> speed{ 1.0f };
	};
}
