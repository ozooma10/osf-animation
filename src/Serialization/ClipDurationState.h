#pragma once

#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace OSF::Serialization::ClipDurations::Detail
{
	inline constexpr float kExactConfirmTolerance = 0.05f;
	inline bool PlausibleSec(float a_sec)
	{
		return std::isfinite(a_sec) && a_sec > 0.0f && a_sec <= 3600.0f;
	}

	struct Entry
	{
		float sec = 0.0f;
		std::uint64_t size = 0;  // 0 = decoded without source identity
		std::int64_t mtime = 0; // file_clock ticks; 0 = not a loose file
		bool exact = false;
		std::uint64_t decodedRevision = 0; // process-local; protects records made during a probe
	};

	struct State
	{
		std::mutex lock; // Memory operations only; never held across IO or JSON serialization.
		std::unordered_map<std::string, Entry> entries;
		std::uint64_t decodedRevision = 0;
		std::uint64_t revision = 0;
		std::uint64_t savedRevision = 0;
	};

	State& GetState();
	std::string KeyFor(std::string_view a_fileSpec, std::string_view a_animId);
}
