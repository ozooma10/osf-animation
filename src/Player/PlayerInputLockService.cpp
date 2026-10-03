#include "Util/NativeMainThreadQueue.h"
#include "Player/PlayerInputLockService.h"

namespace OSF::Player
{
	namespace
	{
		// POVSwitch stays enabled: the mouse wheel rides the POV control, so blocking it kills scroll-zoom.
		// CameraService::Tick bounces a full zoom-in back to third person.
		constexpr auto kSceneUserEvents =
			RE::USER_EVENT_FLAG::Movement |
			RE::USER_EVENT_FLAG::Fighting |
			RE::USER_EVENT_FLAG::Sneaking |
			RE::USER_EVENT_FLAG::Activation;

		constexpr auto kSceneOtherEvents =
			RE::OTHER_EVENT_FLAG::Activate |
			RE::OTHER_EVENT_FLAG::Running |
			RE::OTHER_EVENT_FLAG::Sprinting |
			RE::OTHER_EVENT_FLAG::HandScanner |
			RE::OTHER_EVENT_FLAG::Favorites;

		// AI-driven persists in saves; only cleared here, never set (it blocks camera look). Via the Papyrus VM, any thread.
		void SetPlayerAIDriven(bool a_driven)
		{
			Util::NativeMainThreadQueue::Post([a_driven]() {
				auto* game = RE::GameVM::GetSingleton();
				auto* vm = game ? game->GetVM() : nullptr;
				if (!vm) {
					return;
				}
				const bool dispatched = vm->DispatchStaticCall(
					"Game", "SetPlayerAIDriven",
					[&](RE::BSScrapArray<RE::BSScript::Variable>& a_args) {
						a_args.clear();
						a_args.resize(1);
						RE::BSScript::PackVariable(a_args[0], a_driven);
						return true;
					},
					RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>(), 0);
				if (!dispatched) {
					REX::TRACE("[Player] Game.SetPlayerAIDriven({}) dispatch failed (VM rebuilding mid-load?) -- player AI-driven state may lag until the next load", a_driven);
				}
			});
		}
	}

	PlayerInputLockService::PlayerInputLockService() :
		userMask(static_cast<uint32_t>(kSceneUserEvents)),
		otherMask(static_cast<uint32_t>(kSceneOtherEvents))
	{}

	PlayerInputLockService& PlayerInputLockService::GetSingleton()
	{
		static PlayerInputLockService instance;
		return instance;
	}

	void PlayerInputLockService::OnStopAll()
	{
		const auto epoch = taskEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
		// OnStopAll runs only on a load; clear a persisted AI-driven flag (the input layer itself is non-persistent).
		SetPlayerAIDriven(false);

		Util::NativeMainThreadQueue::Post([this, epoch]() {
			if (taskEpoch.load(std::memory_order_acquire) != epoch) {
				return;
			}
			std::scoped_lock l{ lock };
			if (playerInputLockActive) {
				playerInputLockActive = false;
				RestoreEnabled();
				REX::DEBUG("[Player] input lock restored for StopAll");
			}
		});
	}

	void PlayerInputLockService::ClearAIDriven()
	{
		SetPlayerAIDriven(false);
	}

	void PlayerInputLockService::SetPlayerInputLock(bool a_enable)
	{
		const auto epoch = taskEpoch.load(std::memory_order_acquire);
		Util::NativeMainThreadQueue::Post([this, a_enable, epoch]() {
			if (taskEpoch.load(std::memory_order_acquire) != epoch) {
				return;
			}
			std::scoped_lock l{ lock };
			if (a_enable) {
				if (playerInputLockActive) {
					return;
				}
				if (!EnsureLayer()) {
					REX::WARN("[Player] input lock: failed to allocate input-disable layer");
					return;
				}
				playerInputLockActive = true;
				ApplyDisabled();
				// Movement/Fighting/Sneaking/Activation only; looking stays free.
				REX::DEBUG("[Player] input lock engaged (movement only -- camera look stays free)");
			} else {
				if (!playerInputLockActive) {
					return;
				}
				playerInputLockActive = false;
				RestoreEnabled();
				// tfc sets AI-driven and toggling it off doesn't reliably clear it for a pinned participant (player can't move).
				SetPlayerAIDriven(false);
				REX::DEBUG("[Player] input lock released");
			}
		});
	}

	bool PlayerInputLockService::EnsureLayer()
	{
		if (inputLayer) {
			return true;
		}

		auto* manager = RE::BSInputEnableManager::GetSingleton();
		if (!manager) {
			return false;
		}

		RE::BSInputEnableLayer* layer = nullptr;
		if (!manager->AllocateNewLayer(&layer, "OSF player input lock")) {
			return false;
		}

		inputLayer = layer;
		return inputLayer != nullptr;
	}

	void PlayerInputLockService::ApplyDisabled()
	{
		if (!inputLayer) {
			return;
		}
		if (userMask) {
			inputLayer->EnableUserEvent(static_cast<RE::USER_EVENT_FLAG>(userMask), false, RE::USER_EVENT_SENDER_ID::Script);
		}
		if (otherMask) {
			inputLayer->EnableOtherEvent(static_cast<RE::OTHER_EVENT_FLAG>(otherMask), false, RE::USER_EVENT_SENDER_ID::Script);
		}
	}

	void PlayerInputLockService::RestoreEnabled()
	{
		if (!inputLayer) {
			return;
		}
		if (userMask) {
			inputLayer->EnableUserEvent(static_cast<RE::USER_EVENT_FLAG>(userMask), true, RE::USER_EVENT_SENDER_ID::Script);
		}
		if (otherMask) {
			inputLayer->EnableOtherEvent(static_cast<RE::OTHER_EVENT_FLAG>(otherMask), true, RE::USER_EVENT_SENDER_ID::Script);
		}
	}
}
