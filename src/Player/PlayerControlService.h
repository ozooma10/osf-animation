#pragma once

namespace OSF::Player
{
	// Standalone player-control lock used by the SAF-compat playback path (the
	// Play+Sync flow that has no managed Scene). Engages an input-disable layer
	// (Movement incl. Jumping, Fighting, Sneaking, Activation, ...) plus the
	// persistent AI-driven flag (decouples the body from the camera so a pinned
	// rig doesn't yaw with mouse-look). Scene-integrated control policy lives in
	// the OSF Intimacy scene engine, not here.
	class PlayerControlService
	{
	public:
		static PlayerControlService& GetSingleton();

		// Save/load teardown: release the lock and clear the persistent AI-driven
		// flag UNCONDITIONALLY (it serializes into saves, unlike the runtime input
		// layer, so a save made while held reloads AI-driven with no in-process
		// memory of the lock).
		void OnStopAll();

		// Debug/bisect: replace the user/other disable masks at runtime (queued
		// to the game thread; live-reapplied if a lock is active). The CLSF flag
		// names are marked unconfirmed, so when an input we want alive (e.g.
		// scroll-zoom) turns out gated by a bit we disable, this lets one game
		// session bisect the real bit layout from the console.
		void SetMasks(uint32_t a_userMask, uint32_t a_otherMask);

		// Standalone player control lock: engages the input-disable layer + masks
		// (Movement includes Jumping, which AI-driven alone leaks) plus AI-driven.
		// Released by the matching false call or OnStopAll. Idempotent.
		void SetStandaloneLock(bool a_enable);

	private:
		PlayerControlService();

		bool EnsureLayer();
		void ApplyDisabled();
		void RestoreEnabled();

		std::mutex lock;
		RE::BSInputEnableLayer* inputLayer = nullptr;
		bool standaloneActive = false;
		// Masks currently configured (what ApplyDisabled will disable) and the
		// masks actually applied to the live layer (what RestoreEnabled must
		// re-enable — kept separate so a mid-lock SetMasks can't strand bits).
		uint32_t userMask = 0;
		uint32_t otherMask = 0;
		uint32_t appliedUserMask = 0;
		uint32_t appliedOtherMask = 0;
	};
}
