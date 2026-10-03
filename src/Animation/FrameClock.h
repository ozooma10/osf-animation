#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace OSF::Animation
{
	// A clock owner that hasn't advanced within this window is treated as unloaded.
	inline constexpr std::int64_t kClockOwnerStaleMs = 250;

	// Engine calls Update several times per frame per graph with subdivided dt, possibly from several
	// managers; only the owning manager advances time. Ownership moves on after kClockOwnerStaleMs.
	struct FrameClock
	{
		const void* owner = nullptr;
		float time = 0.0f;
		std::int64_t lastAdvanceMs = 0;

		bool ShouldAdvance(const void* a_token)
		{
			const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			return ShouldAdvance(a_token, nowMs);
		}

		bool ShouldAdvance(const void* a_token, std::int64_t a_nowMs)
		{
			if (owner && owner != a_token && a_nowMs - lastAdvanceMs > kClockOwnerStaleMs) {
				owner = nullptr;
			}
			if (!owner) {
				owner = a_token;
			}
			if (a_token != owner) {
				return false;
			}
			lastAdvanceMs = a_nowMs;
			return true;
		}

		void Reset()
		{
			owner = nullptr;
			time = 0.0f;
			lastAdvanceMs = 0;
		}
	};

	// Not wrapped: each graph wraps by its own clip duration, so mismatched lengths share a phase origin.
	struct SyncGroup
	{
		std::mutex lock;
		FrameClock clock;
		
		// Set by any member's SetSpeed; applies regardless of which graph owns the clock.
		std::atomic<float> speed{ 1.0f };
	};
}
