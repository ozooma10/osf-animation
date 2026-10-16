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

		// Also clears the AI-driven flag unconditionally after world replacement.
		void OnStopAll();

		// Game-thread save events: keep this layer's disabled bits out of the vanilla save.
		void OnSaveBegin();
		void OnSaveEnd();

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
		bool saveSuspended = false;
		// kSceneUserEvents / kSceneOtherEvents, fixed at construction.
		const uint32_t userMask;
		const uint32_t otherMask;
	};
}
