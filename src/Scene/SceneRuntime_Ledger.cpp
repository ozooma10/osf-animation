#include "Scene/SceneRuntime.h"

#include "Camera/CameraService.h"
#include "Input/InputService.h"
#include "Player/PlayerInputLockService.h"
#include "Props/PropService.h"
#include "UI/FadeService.h"
#include "Weapon/WeaponService.h"

#include <algorithm>

// The player input lock needs no ref-count: MintSlot exclusivity means at most one live scene holds the player.
namespace OSF::Scene
{
	void SceneRuntime::RecordMechanism(std::int32_t a_handle, Mechanism a_mech)
	{
		bool engagePlayerInputLock = false;
		bool engageCamera = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return;
			}
			if (std::find(s->ledger.begin(), s->ledger.end(), a_mech) != s->ledger.end()) {
				return;
			}
			s->ledger.push_back(a_mech);
			if (a_mech == Mechanism::kPlayerInputLock) {
				engagePlayerInputLock = true;
			} else if (a_mech == Mechanism::kCamera) {
				engageCamera = true;  // ref-counted by CameraService
			}
			// kFade: RunAction already posted the fade-out.
		}
		if (engagePlayerInputLock) {
			Player::PlayerInputLockService::GetSingleton().SetPlayerInputLock(true);
			Camera::CameraService::GetSingleton().SetStandaloneLock(true);
		}
		if (engageCamera) {
			Camera::CameraService::GetSingleton().SetStandaloneLock(true);
		}
	}

	void SceneRuntime::UndoMechanism(std::int32_t a_handle, Mechanism a_mech)
	{
		ActorState::Handle scope{};
		std::vector<RE::Actor*> weapon;
		std::vector<ActiveProp> props;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return;
			}
			const auto it = std::find(s->ledger.begin(), s->ledger.end(), a_mech);
			if (it == s->ledger.end()) {
				return;
			}
			s->ledger.erase(it);
			scope = s->equipmentScope;
			if (a_mech == Mechanism::kWeapon) {
				weapon.swap(s->sheathedWeapon);
			} else if (a_mech == Mechanism::kProps) {
				props.swap(s->props);
			} else if (a_mech == Mechanism::kInputChannel) {
				s->grant = {};
				++s->grantRevision;
			}
		}
		// Reverse outside _lock: services enter the VM, post UI messages, or take the inventory lock.
		switch (a_mech) {
		case Mechanism::kPlayerInputLock:
			REX::TRACE("[Scene] scene {:#010x} player input lock released — vanilla input restored", a_handle);
			Player::PlayerInputLockService::GetSingleton().SetPlayerInputLock(false);
			Camera::CameraService::GetSingleton().SetStandaloneLock(false);
			break;
		case Mechanism::kFade:
			REX::TRACE("[Scene] scene {:#010x} fade undo — fading back in", a_handle);
			UI::FadeService::GetSingleton().FadeFromBlack(0.5f);
			break;
		case Mechanism::kEquipment:
			ActorState::Report(ActorState::Get().ReleaseGroup(scope, ActorState::Apparel), "release apparel");
			break;
		case Mechanism::kHeldEquipment:
			ActorState::Report(ActorState::Get().ReleaseGroup(scope, ActorState::Held), "release held items");
			break;
		case Mechanism::kEquipItem:
			ActorState::Report(ActorState::Get().ReleaseGroup(scope, ActorState::Authored), "release authored items");
			break;
		case Mechanism::kCamera:
			REX::TRACE("[Scene] scene {:#010x} camera undo — releasing the camera hold", a_handle);
			Camera::CameraService::GetSingleton().SetStandaloneLock(false);
			break;
		case Mechanism::kCameraState:
			REX::TRACE("[Scene] scene {:#010x} camera-state undo — releasing the camera override", a_handle);
			Camera::CameraService::GetSingleton().ReleaseStateOverride();
			break;
		case Mechanism::kWeapon:
			REX::TRACE("[Scene] scene {:#010x} weapon undo — re-drawing {} actor(s)", a_handle, weapon.size());
			for (auto* actor : weapon) {
				Weapon::WeaponService::GetSingleton().Draw(actor);
			}
			break;
		case Mechanism::kInputChannel:
			REX::TRACE("[Scene] scene {:#010x} input channel undo — releasing the director grant", a_handle);
			Input::InputService::GetSingleton().Release(a_handle);
			// Free cam must not outlive the grant that enabled it. Runs before the input-lock undo so
			// the camera hands back to the still-armed third-person hold.
			Camera::CameraService::GetSingleton().ForcePlayerFreeCamOff();
			break;
		case Mechanism::kProps:
			REX::TRACE("[Scene] scene {:#010x} prop undo — destroying {} prop(s)",
				a_handle, props.size());
			for (auto& prop : props) {
				std::string error;
				if (!Props::PropService::GetSingleton().Destroy(
						prop.instance, &error)) {
					REX::ERROR("[Scene] scene {:#010x} prop cleanup '{}' failed: {}",
						a_handle, prop.id, error);
				}
			}
			break;
		}
	}

	void SceneRuntime::ReplayLedger(std::int32_t a_handle)
	{
		std::vector<Mechanism> reversed;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return;
			}
			reversed.assign(s->ledger.rbegin(), s->ledger.rend());
		}
		for (auto m : reversed) {
			UndoMechanism(a_handle, m);
		}
		ActorState::Report(ActorState::Get().CloseSession(static_cast<std::uint32_t>(a_handle)), "end session");
	}

	bool SceneRuntime::HideEquipment(std::int32_t a_handle, RE::Actor* a_actor, std::uint32_t a_slotMask)
	{
		if (!a_handle || !a_actor || a_slotMask == 0) {
			return false;
		}
		if (GetSceneForActor(a_actor) != a_handle) {
			REX::DEBUG("[Scene] scene {:#010x} HideEquipment: actor {:X} is not a participant", a_handle, a_actor->formID);
			return false;
		}
		return ApplyHiddenEquipment(a_handle, a_actor, a_slotMask, false) != 0;
	}

	bool SceneRuntime::RestoreHiddenEquipment(std::int32_t a_handle)
	{
		{
			std::lock_guard l{ _lock };
			if (!Resolve(a_handle)) {
				return false;
			}
		}
		UndoMechanism(a_handle, Mechanism::kEquipment);
		return true;
	}

	ActorState::Handle SceneRuntime::EquipmentScope(std::int32_t a_handle)
	{
		std::lock_guard l{ _lock };
		auto* slot = Resolve(a_handle);
		if (!slot) return 0;
		if (!ActorState::Get().HasScope(slot->equipmentScope))
			slot->equipmentScope = ActorState::Get().CreateScope("osf.animation.scene", ActorState::Lifetime::Session,
				static_cast<std::uint32_t>(a_handle));
		return slot->equipmentScope;
	}

	std::size_t SceneRuntime::ApplyHiddenEquipment(std::int32_t a_handle, RE::Actor* actor,
		std::uint32_t slots, bool held, std::span<RE::TESBoundObject* const> keep)
	{
		const auto scope = EquipmentScope(a_handle);
		const auto count = ActorState::Hide(scope, actor, slots, held, keep);
		if (count) RecordMechanism(a_handle, held ? Mechanism::kHeldEquipment : Mechanism::kEquipment);
		return count;
	}

	bool SceneRuntime::ApplyEquippedItem(std::int32_t a_handle, RE::Actor* actor, RE::TESBoundObject* object, bool gear)
	{
		const auto scope = EquipmentScope(a_handle);
		const auto result = ActorState::Acquire(scope, actor, object, ActorState::Operation::Equip,
			gear ? ActorState::Gear : ActorState::Authored, !gear, !gear);
		if (result) RecordMechanism(a_handle, Mechanism::kEquipItem);
		return static_cast<bool>(result);
	}

	void SceneRuntime::RecordSheathedWeapon(std::int32_t a_handle, RE::Actor* a_actor)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_handle);
		if (!s) {
			return;
		}
		if (std::find(s->sheathedWeapon.begin(), s->sheathedWeapon.end(), a_actor) != s->sheathedWeapon.end()) {
			return;
		}
		s->sheathedWeapon.push_back(a_actor);
		if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kWeapon) == s->ledger.end()) {
			s->ledger.push_back(Mechanism::kWeapon);
		}
	}

	bool SceneRuntime::RecordCameraState(std::int32_t a_handle, Camera::CameraMode a_mode)
	{
		auto& cam = Camera::CameraService::GetSingleton();
		if (a_mode == Camera::CameraMode::kSceneOrbit && !cam.SceneOrbitAvailable()) {
			return false;  // keep the engine-owned ship camera
		}
		bool firstForScene = false;
		std::vector<std::uint32_t> cast;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return false;
			}
			if (a_mode == Camera::CameraMode::kSceneOrbit) {
				// Form IDs, so the service frames the cast at live post-placement positions.
				for (const auto* actor : s->participants) {
					if (actor) {
						cast.push_back(actor->formID);
					}
				}
			}
			if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kCameraState) == s->ledger.end()) {
				s->ledger.push_back(Mechanism::kCameraState);
				firstForScene = true;
			}
		}
		if (a_mode == Camera::CameraMode::kSceneOrbit) {
			cam.SetOrbitFrameSubjects(std::move(cast));
		}
		if (firstForScene) {
			cam.AcquireStateOverride();
		}
		cam.SetLiveCameraState(a_mode);  // every call, for per-node switches
		return true;
	}

	void SceneRuntime::RecordInputChannel(std::int32_t a_handle, const Input::Grant& a_grant)
	{
		Input::Grant grant;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return;
			}
			s->grant = a_grant;
			s->grant.revision = ++s->grantRevision;
			if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kInputChannel) == s->ledger.end()) {
				s->ledger.push_back(Mechanism::kInputChannel);
			}
			grant = s->grant;
		}
		Input::InputService::GetSingleton().Engage(grant);
	}
}
