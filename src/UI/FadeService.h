#pragma once

// Native screen fade-to-black, the engine-side equivalent of SAF Seduce's
// ImageSpaceModifier fade. We post through the engine function behind the
// Papyrus native Game.FadeOutGame — no plugin records needed, and the fade
// rides the game's own FaderMenu (engine-paced, save-load aware UI path).
// Full RE record: osf-re ui.fader_menu (static trace + runtime proof,
// 2026-06-12, 1.16.244; poster bytes identical on 1.16.242).
//
// The poster (AddrLib 114430) is build-request + enqueue onto the spin-locked
// UIMessageQueue — RE-verdict: callable from ANY thread, so scene start/stop
// and job-thread ticks need no marshaling.
//
// CRASH CONSTRAINT (runtime-proven): a held stay-faded latch (FaderMenu
// +0x19B) across a save-load CRASHES the engine. Every hold this service
// creates is therefore (a) deadline-bounded — Tick() force-releases it — and
// (b) released immediately by OnStopAll(), which GraphManager::StopAll calls
// synchronously from the SaveLoadEvent BEGIN sink before the load proceeds.
// Never add a hold path that bypasses the release deadline.
//
// Lean carve: the scene runtime drives fades explicitly through the
// osf.fade.out/osf.fade.in actions + the undo ledger (SceneRuntime), so this
// service is just the content-neutral fade mechanism — no Scene* coupling.

namespace OSF::UI
{
	class FadeService
	{
	public:
		static FadeService& GetSingleton();

		// True when the fade poster's prologue matches the RE-verified bytes
		// on this runtime. Computed once; a mismatch logs and self-disables.
		bool Available();

		// Auto-fade toggle (user setting). When false, osf.fade.* actions
		// silent-skip (§1.5); the manual entry points still honor it.
		void SetEnabled(bool a_enabled);
		bool Enabled() const;

		// Fade to black and hold. The hold is CAPPED — Tick() posts the fade-in
		// at a_fadeSecs + a_holdMaxSecs even if nothing releases it (crash
		// safety). Returns false when unavailable.
		bool FadeToBlack(float a_fadeSecs, float a_holdMaxSecs);

		// Fade back in, releasing the engine-side stay-faded latch. Returns
		// false when unavailable.
		bool FadeFromBlack(float a_fadeSecs);

		// Save-load teardown: release any held/pending fade NOW (see the crash
		// constraint above). Called synchronously from GraphManager::StopAll.
		void OnStopAll();

		// Rides the update-hook call stream (job threads): posts the deferred
		// fade-in once the hold deadline passes. Atomic early-out when idle.
		void Tick();

	private:
		void PostFade(bool a_fadingOut, float a_fadeSecs, bool a_stayFaded);
		void ScheduleRelease(float a_fadeSecs, float a_holdSecs);

		std::atomic<bool> enabled{ true };
		// Steady-clock ms deadline for the deferred fade-in; 0 = nothing held.
		std::atomic<int64_t> releaseAtMs{ 0 };
	};
}
