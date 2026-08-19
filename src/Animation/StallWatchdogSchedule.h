#pragma once

#include <cstdint>

namespace OSF::Animation
{
	// Pure timing policy for GraphManager's main-thread stall scan. Keeping it separate makes the pause/resume grace and scan throttle testable without a game runtime.
	class StallWatchdogSchedule
	{
	public:
		static constexpr std::int64_t kResumeGapMs = 500;
		static constexpr std::int64_t kGraceMs = 2000;
		static constexpr std::int64_t kScanIntervalMs = 250;

		void Pause() { _lastBeatMs = 0; }

		bool ShouldScan(std::int64_t a_nowMs)
		{
			const std::int64_t previous = _lastBeatMs;
			_lastBeatMs = a_nowMs;
			if (previous == 0 || a_nowMs - previous > kResumeGapMs) {
				_armedAtMs = a_nowMs + kGraceMs;
				return false;
			}
			if (a_nowMs < _armedAtMs || a_nowMs - _lastScanMs < kScanIntervalMs) {
				return false;
			}
			_lastScanMs = a_nowMs;
			return true;
		}

	private:
		std::int64_t _lastBeatMs{ 0 };
		std::int64_t _armedAtMs{ 0 };
		std::int64_t _lastScanMs{ 0 };
	};
}
