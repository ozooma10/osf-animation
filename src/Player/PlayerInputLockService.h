#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace OSF::Player
{
	// Player input lock used by the scene runtime's default playerInputLock policy and
	// osf.control.lock action.
	// Engages an input-disable layer (Movement incl. Jumping, Fighting, Sneaking, Activation, ...).
	// This is only the vanilla-input suppression mechanism; SceneControls governs OSF commands.
	class PlayerInputLockService
	{
	public:
		static PlayerInputLockService& GetSingleton();

		// Save/load teardown: release the input lock and clear the persistent AI-driven flag
		// unconditionally (it serializes into saves, unlike the runtime input layer).
		void OnStopAll();

		// Engages the player input-disable layer and masks. Released by the matching false call
		// or OnStopAll. Idempotent.
		void SetPlayerInputLock(bool a_enable);

		// Compatibility spelling retained for existing native callers.
		void SetStandaloneLock(bool a_enable) { SetPlayerInputLock(a_enable); }

		// Clears the persistent AI-driven flag without touching the player input lock.
		// Used after a load and after native free cam (tfc), which can leave it set.
		void ClearAIDriven();

	private:
		PlayerInputLockService();

		bool EnsureLayer();
		void ApplyDisabled();
		void RestoreEnabled();

		std::mutex lock;
		std::atomic<std::uint64_t> taskEpoch{ 1 };
		RE::BSInputEnableLayer* inputLayer = nullptr;
		bool playerInputLockActive = false;
		// The input events disabled by the lock, fixed at construction (kSceneUserEvents / kSceneOtherEvents).
		// ApplyDisabled disables them; RestoreEnabled re-enables them.
		const uint32_t userMask;
		const uint32_t otherMask;
	};
}
