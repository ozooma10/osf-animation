#include "Player/PlayerControlService.h"

namespace OSF::Player
{
	namespace
	{
		// POVSwitch stays ENABLED: in Starfield the mouse wheel rides the POV control (one continuous first<->third zoom axis), so blocking it kills scroll-zoom during the lock.
		// CameraService::Tick bounces the camera back to third person if the player zooms/keys all the way into first person.
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

		// Clears the player's AI-driven flag via the Papyrus VM (Game.SetPlayerAIDriven). The lock NO LONGER
		// engages AI-driven — it blocked camera look (an AI-driven actor is non-controllable) WITHOUT decoupling
		// the rig (GraphManager's compose-root rotation pin does that). This remains only to clear a STALE flag:
		// AI-driven is PERSISTENT (serialized into saves), so a save made by an older build that set it would
		// reload AI-driven; OnStopAll clears it on every load. No native binding; queued via SFSE task, any-thread.
		void SetPlayerAIDriven(bool a_driven)
		{
			SFSE::GetTaskInterface()->AddTask([a_driven]() {
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
					REX::TRACE("[Player] Game.SetPlayerAIDriven({}) dispatch failed (VM rebuilding mid-load?) — player AI-driven state may lag until the next load", a_driven);
				}
			});
		}
	}

	PlayerControlService::PlayerControlService() :
		userMask(static_cast<uint32_t>(kSceneUserEvents)),
		otherMask(static_cast<uint32_t>(kSceneOtherEvents))
	{}

	PlayerControlService& PlayerControlService::GetSingleton()
	{
		static PlayerControlService instance;
		return instance;
	}

	void PlayerControlService::OnStopAll()
	{
		// Defensively clear the PERSISTENT AI-driven flag on every load. The lock no longer sets it, but a save
		// made by an OLDER build (which did) would reload the player AI-driven with no in-process memory of it,
		// leaving them non-controllable. OnStopAll runs only on a load (every GraphManager::StopAll caller is a
		// save-load/revert/manual-load sink). The runtime input-disable layer below is non-persistent.
		SetPlayerAIDriven(false);

		SFSE::GetTaskInterface()->AddTask([this]() {
			std::scoped_lock l{ lock };
			if (standaloneActive) {
				standaloneActive = false;
				RestoreEnabled();
				REX::DEBUG("[Player] control lock restored for StopAll");
			}
		});
	}

	void PlayerControlService::SetStandaloneLock(bool a_enable)
	{
		SFSE::GetTaskInterface()->AddTask([this, a_enable]() {
			std::scoped_lock l{ lock };
			if (a_enable) {
				if (standaloneActive) {
					return;  // already held — idempotent
				}
				if (!EnsureLayer()) {
					REX::WARN("[Player] standalone control lock: failed to allocate input-disable layer");
					return;
				}
				standaloneActive = true;
				ApplyDisabled();
				// No AI-driven: it blocked camera look (an AI-driven actor is non-controllable) without
				// actually decoupling the rig — the GraphManager compose-root rotation pin handles rig-spin.
				// The input layer disables Movement/Fighting/Sneaking/Activation only; Looking stays free.
				REX::DEBUG("[Player] standalone control lock engaged (movement only — camera look stays free)");
			} else {
				if (!standaloneActive) {
					return;  // not held — nothing to release
				}
				standaloneActive = false;
				RestoreEnabled();
				// Clear any persistent AI-driven flag at the player-unlock point. The lock itself never
				// sets it, BUT the native free cam (MMB -> ToggleFreeCameraMode / `tfc`) does — to freeze
				// the player while the camera flies — and toggling tfc back off doesn't reliably clear it
				// for a pinned scene participant. Without this, that flag survives SCENE_END (it's only
				// otherwise cleared on a game load via OnStopAll), leaving the player unlocked at OSF's
				// level but still AI-driven (= unable to move). Idempotent: a no-op when nothing set it.
				SetPlayerAIDriven(false);
				REX::DEBUG("[Player] standalone control lock released");
			}
		});
	}

	bool PlayerControlService::EnsureLayer()
	{
		if (inputLayer) {
			return true;
		}

		auto* manager = RE::BSInputEnableManager::GetSingleton();
		if (!manager) {
			return false;
		}

		RE::BSInputEnableLayer* layer = nullptr;
		if (!manager->AllocateNewLayer(&layer, "OSF player scene")) {
			return false;
		}

		inputLayer = layer;
		return inputLayer != nullptr;
	}

	void PlayerControlService::ApplyDisabled()
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

	void PlayerControlService::RestoreEnabled()
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
