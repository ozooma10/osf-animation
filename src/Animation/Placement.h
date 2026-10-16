#pragma once

#include <cmath>

namespace OSF::Animation
{
	// Metres in the scene anchor's heading frame; heading is relative radians.
	struct ParticipantPlacement
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float heading = 0.0f;
	};

	inline ParticipantPlacement AddPlacement(const ParticipantPlacement& a_base, const ParticipantPlacement& a_delta)
	{
		return {a_base.x + a_delta.x, a_base.y + a_delta.y, a_base.z + a_delta.z, a_base.heading + a_delta.heading};
	}

	inline bool ValidAlignmentOffset(const ParticipantPlacement& a_offset)
	{
		return std::isfinite(a_offset.x) && std::isfinite(a_offset.y) && std::isfinite(a_offset.z) &&
			std::isfinite(a_offset.heading) && std::abs(a_offset.x) <= 5.0f && std::abs(a_offset.y) <= 5.0f &&
			std::abs(a_offset.z) <= 5.0f && std::abs(a_offset.heading) <= 3.141593f;
	}
}
