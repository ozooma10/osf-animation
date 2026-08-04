#pragma once

#include "Animation/Scene.h"

#include <cstdint>
#include <vector>

namespace OSF::Animation
{
	enum class PlaybackOccupant : std::uint8_t
	{
		kEmpty,
		kStandalone,
		kStaged
	};

	enum class PlaybackAdmissionReason : std::uint8_t
	{
		kAccepted,
		kExpectedMissing,
		kPlaybackMismatch,
		kStandaloneOccupied,
		kOwnerMismatch
	};

	struct PlaybackClaim
	{
		PlaybackOccupant occupant = PlaybackOccupant::kEmpty;
		PlaybackId playbackId = 0;
		PlaybackSinkId sinkId = 0;
	};

	struct PlaybackAdmission
	{
		bool accepted = false;
		bool replace = false;
		PlaybackAdmissionReason reason = PlaybackAdmissionReason::kExpectedMissing;
	};

	inline PlaybackAdmission EvaluatePlaybackAdmission(const PlaybackClaim& a_current,
		PlaybackId a_expectedPlayback, PlaybackSinkId a_requestingSink) noexcept
	{
		if (a_current.occupant == PlaybackOccupant::kEmpty) {
			return a_expectedPlayback == 0 ?
				PlaybackAdmission{ true, false, PlaybackAdmissionReason::kAccepted } :
				PlaybackAdmission{ false, false, PlaybackAdmissionReason::kExpectedMissing };
		}
		if (a_current.occupant == PlaybackOccupant::kStandalone) {
			return a_expectedPlayback == 0 && a_requestingSink == 0 ?
				PlaybackAdmission{ true, false, PlaybackAdmissionReason::kAccepted } :
				PlaybackAdmission{ false, false, PlaybackAdmissionReason::kStandaloneOccupied };
		}
		if (a_expectedPlayback == 0 || a_current.playbackId != a_expectedPlayback) {
			return { false, false, PlaybackAdmissionReason::kPlaybackMismatch };
		}
		if (a_current.sinkId != a_requestingSink) {
			return { false, false, PlaybackAdmissionReason::kOwnerMismatch };
		}
		return { true, true, PlaybackAdmissionReason::kAccepted };
	}

	inline const TimedMark* FirstInvalidStrictTimedMark(const std::vector<TimedMark>& a_marks,
		float a_duration) noexcept
	{
		for (const auto& mark : a_marks) {
			if (!mark.atEnd && MarkTime(mark, a_duration) >= a_duration) return &mark;
		}
		return nullptr;
	}
}
