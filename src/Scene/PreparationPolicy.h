#pragma once

#include "Preparation/Coordinator.h"

namespace OSF::Scene
{
	using TransitionOrigin = Preparation::Origin;

	constexpr bool IsAutomaticTransition(TransitionOrigin origin)
	{
		return origin == TransitionOrigin::TimedCue || origin == TransitionOrigin::PlaybackCompletion;
	}

	// Attempt zero denotes an already committed lease. A refusal before coordinator
	// admission has no attempt and therefore owns no reservations to roll back.
	constexpr bool OwnsPendingResource(Preparation::Token leaseAttempt, Preparation::Token attempt)
	{
		return attempt != 0 && leaseAttempt == attempt;
	}

	// An automatic boundary is consumed once. Resuming after refusal would strand a
	// one-shot cue or completed stage; only untouched manual attempts may resume.
	constexpr bool MayResumePreparedSource(Preparation::Kind kind, bool automatic,
		bool cancelled, bool applied, bool exitFired, bool restored)
	{
		return kind != Preparation::Kind::Start && !automatic && !cancelled &&
		       !applied && !exitFired && restored;
	}
}
