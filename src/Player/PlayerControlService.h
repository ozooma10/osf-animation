#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace OSF::Player
{
	// Standalone player-control lock applied by the scene runtime's osf.control.lock action.
	// Engages an input-disable layer (Movement incl. Jumping, Fighting, Sneaking, Activation, ...).
	// This is just the lock mechanism; the scene runtime decides when to apply it.
	class PlayerControlService
	{
	public:
		static PlayerControlService& GetSingleton();

		// Save/load teardown: release the lock and clear the persistent AI-driven flag
		// unconditionally (it serializes into saves, unlike the runtime input layer).
		void OnStopAll();

		// Standalone player control lock: engages the input-disable layer + masks.
		// Released by the matching false call or OnStopAll. Idempotent.
		void SetStandaloneLock(bool a_enable);

		// Clears the persistent AI-driven flag without touching the control lock.
		// Used after a load and after native free cam (tfc), which can leave it set.
		void ClearAIDriven();

	private:
		PlayerControlService();

		bool EnsureLayer();
		void ApplyDisabled();
		void RestoreEnabled();

		std::mutex lock;
		std::atomic<std::uint64_t> taskEpoch{ 1 };
		RE::BSInputEnableLayer* inputLayer = nullptr;
		bool standaloneActive = false;
		// The input events the lock disables, fixed at construction (kSceneUserEvents / kSceneOtherEvents).
		// ApplyDisabled disables them; RestoreEnabled re-enables them.
		const uint32_t userMask;
		const uint32_t otherMask;
	};
}
