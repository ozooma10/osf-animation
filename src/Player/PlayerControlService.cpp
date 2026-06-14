#include "Player/PlayerControlService.h"

namespace OSF::Player
{
	namespace
	{
		// POVSwitch stays ENABLED: in Starfield the mouse wheel rides the POV
		// control (one continuous first<->third zoom axis), so blocking it kills
		// scroll-zoom during the lock. CameraService::Tick bounces the camera back
		// to third person if the player zooms/keys all the way into first person.
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

		// Sets/clears the player's AI-driven flag via the Papyrus VM
		// (Game.SetPlayerAIDriven). AI-driven DECOUPLES the player body from the
		// camera: in third person the engine otherwise yaws the actor's heading to
		// follow mouse-look, dragging the rig off a pinned mesh. Engaged alongside
		// the input-disable lock so the camera still orbits freely but the body
		// stays put.
		//
		// CAVEAT — the flag is PERSISTENT (serialized into the save), unlike the
		// runtime input-disable layer. A save made while it is set reloads with the
		// player AI-driven, so OnStopAll clears it UNCONDITIONALLY on every load.
		// There is no native binding for it. Queued via SFSE task; safe from any
		// thread.
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
					REX::WARN("SetPlayerAIDriven: Game.SetPlayerAIDriven({}) dispatch failed "
							  "(VM rebuilding mid-load?) — player AI-driven state may lag until the next load",
						a_driven);
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
		// Release the PERSISTENT AI-driven flag UNCONDITIONALLY — even when this
		// process holds no lock. OnStopAll runs only on a load (every
		// GraphManager::StopAll caller is a save-load/revert/manual-load sink), and
		// the AI-driven flag set by SetStandaloneLock is serialized into the save,
		// so a save made mid-lock reloads the player AI-driven with NO in-process
		// memory of the lock. The runtime input-disable layer below is
		// non-persistent and gated on standaloneActive.
		SetPlayerAIDriven(false);

		SFSE::GetTaskInterface()->AddTask([this]() {
			std::scoped_lock l{ lock };
			if (standaloneActive) {
				standaloneActive = false;
				RestoreEnabled();
				REX::INFO("Player control lock restored for StopAll");
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
					REX::WARN("Player standalone control lock: failed to allocate input-disable layer");
					return;
				}
				standaloneActive = true;
				ApplyDisabled();
				SetPlayerAIDriven(true);
				REX::INFO("Player standalone control lock engaged (+ AI-driven)");
			} else {
				if (!standaloneActive) {
					return;  // not held — nothing to release
				}
				standaloneActive = false;
				RestoreEnabled();
				SetPlayerAIDriven(false);
				REX::INFO("Player standalone control lock released (+ AI-driven off)");
			}
		});
	}

	void PlayerControlService::SetMasks(uint32_t a_userMask, uint32_t a_otherMask)
	{
		SFSE::GetTaskInterface()->AddTask([this, a_userMask, a_otherMask]() {
			std::scoped_lock l{ lock };
			const bool live = standaloneActive && inputLayer;
			if (live) {
				RestoreEnabled();
			}
			userMask = a_userMask;
			otherMask = a_otherMask;
			if (live) {
				ApplyDisabled();
			}
			REX::INFO("Player control masks set: user=0x{:X} other=0x{:X}{}",
				a_userMask, a_otherMask, live ? " (re-applied to live lock)" : "");
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
		appliedUserMask = userMask;
		appliedOtherMask = otherMask;
		if (appliedUserMask) {
			inputLayer->EnableUserEvent(static_cast<RE::USER_EVENT_FLAG>(appliedUserMask), false, RE::USER_EVENT_SENDER_ID::Script);
		}
		if (appliedOtherMask) {
			inputLayer->EnableOtherEvent(static_cast<RE::OTHER_EVENT_FLAG>(appliedOtherMask), false, RE::USER_EVENT_SENDER_ID::Script);
		}
	}

	void PlayerControlService::RestoreEnabled()
	{
		if (!inputLayer) {
			return;
		}
		if (appliedUserMask) {
			inputLayer->EnableUserEvent(static_cast<RE::USER_EVENT_FLAG>(appliedUserMask), true, RE::USER_EVENT_SENDER_ID::Script);
		}
		if (appliedOtherMask) {
			inputLayer->EnableOtherEvent(static_cast<RE::OTHER_EVENT_FLAG>(appliedOtherMask), true, RE::USER_EVENT_SENDER_ID::Script);
		}
		appliedUserMask = 0;
		appliedOtherMask = 0;
	}
}
