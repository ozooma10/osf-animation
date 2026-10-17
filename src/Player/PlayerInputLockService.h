#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace OSF::Player
{
	// Vanilla-input suppression via an input-enable layer; SceneControls governs OSF commands.
	class PlayerInputLockService
	{
	public:
		static PlayerInputLockService& GetSingleton();

		// Discards the old world's lock and clears its AI-driven flag.
		void OnStopAll();

		// Reconcile this service's layer after the engine has restored save state.
		// Preserve any lock acquired for the new world; never touch other layers.
		void OnPostLoad();

		// Idempotent; released by the matching false call or OnStopAll.
		void SetPlayerInputLock(bool a_enable);

		void SetStandaloneLock(bool a_enable) { SetPlayerInputLock(a_enable); }

		// Needed after a load and after tfc, which can leave it set.
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
		// kSceneUserEvents / kSceneOtherEvents, fixed at construction.
		const uint32_t userMask;
		const uint32_t otherMask;
	};
}
