#pragma once

// Fade via the engine function behind Papyrus Game.FadeOutGame (rides FaderMenu). Posting is any-thread.
// Holding the stay-faded latch across a save-load crashes the game: every hold is deadline-bounded (Tick)
// and released by OnStopAll before the load proceeds. Never add a hold path without the deadline.

namespace OSF::UI
{
	class FadeService
	{
	public:
		static FadeService& GetSingleton();

		// Computed once; a poster byte mismatch disables fades.
		bool Available();

		// The hold is capped: Tick() fades in at a_fadeSecs + a_holdMaxSecs regardless.
		bool FadeToBlack(float a_fadeSecs, float a_holdMaxSecs);

		bool FadeFromBlack(float a_fadeSecs);

		// Synchronous from StopAll (see the crash note above).
		void OnStopAll();

		void Tick();

		// Exclusive screen ownership for a covered handoff. Old tokens cannot release a new owner.
		std::uint64_t BeginTransition();
		bool OwnsTransition(std::uint64_t token);
		bool RevealTransition(std::uint64_t token);
		void EndTransition(std::uint64_t token);

#ifdef OSF_ANIMATION_SMOKE
		bool HoldingForSmoke() const { return releaseAtMs.load() != 0; }
#endif

	private:
		void PostFade(bool a_fadingOut, float a_fadeSecs, bool a_stayFaded);
		void ScheduleRelease(float a_fadeSecs, float a_holdSecs);

		// Steady-clock ms; 0 = nothing held.
		std::atomic<int64_t> releaseAtMs{ 0 };
		std::mutex transitionLock;
		std::uint64_t transitionOwner{}, nextTransition{};
	};
}
