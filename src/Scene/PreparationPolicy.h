#pragma once

#include "Preparation/Coordinator.h"

namespace OSF::Scene
{
	enum class TransitionOrigin { Manual, TimedCue, PlaybackCompletion };

	constexpr bool IsAutomaticTransition(TransitionOrigin origin)
	{
		return origin != TransitionOrigin::Manual;
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
