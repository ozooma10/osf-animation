#pragma once

// Native screen fade-to-black. We post through the same engine function that
// backs the Papyrus Game.FadeOutGame native, so no plugin records are needed and
// the fade rides the game's own FaderMenu (engine-paced and save-load aware).
//
// The poster just builds a request and enqueues it onto the UIMessageQueue, and
// it's safe to call from any thread, so scene start/stop and the job-thread ticks
// don't need to marshal onto the game thread.
//
// One sharp edge to respect: holding the engine's stay-faded latch across a
// save-load crashes the game. So every hold here is (a) deadline-bounded — Tick()
// force-releases it — and (b) released immediately by OnStopAll(), which StopAll
// calls synchronously from the save-load sink before the load proceeds. Never add
// a hold path that skips the release deadline.
//
// The scene runtime drives fades explicitly through the osf.fade.out/osf.fade.in
// actions and the undo ledger, so this service is just the fade mechanism with no
// scene coupling of its own.

namespace OSF::UI
{
	class FadeService
	{
	public:
		static FadeService& GetSingleton();

		// True when the fade poster still matches the bytes we expect on this game
		// build. Computed once; a mismatch logs and disables fades.
		bool Available();

		// Fade to black and hold. The hold is CAPPED — Tick() posts the fade-in
		// at a_fadeSecs + a_holdMaxSecs even if nothing releases it (crash
		// safety). Returns false when unavailable.
		bool FadeToBlack(float a_fadeSecs, float a_holdMaxSecs);

		// Fade back in, releasing the engine-side stay-faded latch. Returns
		// false when unavailable.
		bool FadeFromBlack(float a_fadeSecs);

		// Save-load teardown: release any held or pending fade right now (see the
		// crash note above). Called synchronously from StopAll.
		void OnStopAll();

		// Rides the update-hook call stream (job threads): posts the deferred
		// fade-in once the hold deadline passes. Atomic early-out when idle.
		void Tick();

	private:
		void PostFade(bool a_fadingOut, float a_fadeSecs, bool a_stayFaded);
		void ScheduleRelease(float a_fadeSecs, float a_holdSecs);

		// Steady-clock ms deadline for the deferred fade-in; 0 = nothing held.
		std::atomic<int64_t> releaseAtMs{ 0 };
	};
}
