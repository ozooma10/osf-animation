#pragma once

namespace OSF::Player
{
	// Standalone player-control lock used by the SAF-compat playback path (the Play+Sync flow with no managed Scene). 
	// Engages an input-disable layer (Movement incl. Jumping, Fighting, Sneaking, Activation, ...) plus the persistent AI-driven flag, 
	// which decouples the body from the camera so a pinned rig doesn't yaw with mouse-look. 
	// This is just the lock mechanism; the scene runtime decides when to apply it.
	class PlayerControlService
	{
	public:
		static PlayerControlService& GetSingleton();

		// Save/load teardown: release the lock and clear the persistent AI-driven flag UNCONDITIONALLY 
		// (it serializes into saves, unlike the runtime input layer, so a save made while held reloads AI-driven with no in-process memory of the lock).
		void OnStopAll();

		// Standalone player control lock: engages the input-disable layer + masks (Movement includes Jumping, which AI-driven alone leaks) plus AI-driven.
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
		// The input events the lock disables, fixed at construction (kSceneUserEvents / kSceneOtherEvents). 
		// ApplyDisabled disables them; RestoreEnabled re-enables them.
		const uint32_t userMask;
		const uint32_t otherMask;
	};
}
