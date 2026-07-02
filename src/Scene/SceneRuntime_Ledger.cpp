#include "Scene/SceneRuntime.h"

#include "Camera/CameraService.h"
#include "Equipment/EquipmentService.h"
#include "Input/InputService.h"
#include "Player/PlayerControlService.h"
#include "UI/FadeService.h"
#include "Weapon/WeaponService.h"

#include <algorithm>

// SceneRuntime — undo ledger slice (one class, split across translation units; see SceneRuntime.cpp).
// Every reversible side-effect a scene engages is recorded per-handle and replayed in reverse on termination, so cleanup never depends on an authored release. 
// The control lock keeps a cross-scene ref-count (_controlLockCount); equipment/weapon keep their per-actor state on the Slot.

namespace OSF::Scene
{
	void SceneRuntime::RecordMechanism(std::int32_t a_handle, Mechanism a_mech)
	{
		bool engageLock = false;
		bool engageCamera = false;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return;
			}
			if (std::find(s->ledger.begin(), s->ledger.end(), a_mech) != s->ledger.end()) {
				return;  // already recorded — idempotent per scene+mechanism
			}
			s->ledger.push_back(a_mech);
			if (a_mech == Mechanism::kControlLock) {
				engageLock = (++_controlLockCount == 1);  // first global holder engages the lock
			} else if (a_mech == Mechanism::kCamera) {
				engageCamera = true;  // the camera lock is ref-counted internally (composes w/ control lock)
			}
			// kFade: the visible fade-out was posted by RunAction; recording just notes the debt.
		}
		if (engageLock) {
			Player::PlayerControlService::GetSingleton().SetStandaloneLock(true);
			Camera::CameraService::GetSingleton().SetStandaloneLock(true);
		}
		if (engageCamera) {
			Camera::CameraService::GetSingleton().SetStandaloneLock(true);
		}
	}

	void SceneRuntime::UndoMechanism(std::int32_t a_handle, Mechanism a_mech)
	{
		bool disengageLock = false;
		std::int32_t remaining = 0;
		std::vector<std::pair<RE::Actor*, Equipment::Snapshot>> equip;  // moved out for kEquipment
		std::vector<std::pair<RE::Actor*, Equipment::EquippedItem>> equipItems;  // moved out for kEquipItem
		std::vector<RE::Actor*> weapon;                                 // moved out for kWeapon
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return;
			}
			const auto it = std::find(s->ledger.begin(), s->ledger.end(), a_mech);
			if (it == s->ledger.end()) {
				return;  // not held — idempotent (already reversed, or never engaged)
			}
			s->ledger.erase(it);
			if (a_mech == Mechanism::kControlLock) {
				disengageLock = (--_controlLockCount <= 0);  // last holder releases the actual lock
				if (_controlLockCount < 0) {
					_controlLockCount = 0;
				}
				remaining = _controlLockCount;
			} else if (a_mech == Mechanism::kEquipment) {
				equip.swap(s->hiddenEquip);  // take this scene's hidden apparel out for restore
			} else if (a_mech == Mechanism::kEquipItem) {
				equipItems.swap(s->equippedItems);  // take this scene's equipped items out for removal
			} else if (a_mech == Mechanism::kWeapon) {
				weapon.swap(s->sheathedWeapon);  // take this scene's sheathed actors out for re-draw
			}
		}
		// Apply the reversal OUTSIDE the lock (services enter the VM / post UI messages / touch
		// the inventory lock).
		switch (a_mech) {
		case Mechanism::kControlLock:
			if (disengageLock) {
				REX::TRACE("[Scene] scene {:#010x} control lock released — player unlocked", a_handle);
				Player::PlayerControlService::GetSingleton().SetStandaloneLock(false);
				Camera::CameraService::GetSingleton().SetStandaloneLock(false);
			} else {
				REX::TRACE("[Scene] scene {:#010x} control lock released — {} scene(s) still hold it", a_handle, remaining);
			}
			break;
		case Mechanism::kFade:
			REX::TRACE("[Scene] scene {:#010x} fade undo — fading back in", a_handle);
			UI::FadeService::GetSingleton().FadeFromBlack(0.5f);
			break;
		case Mechanism::kEquipment:
			REX::TRACE("[Scene] scene {:#010x} equipment undo — restoring {} actor(s)", a_handle, equip.size());
			for (auto& [actor, snap] : equip) {
				Equipment::EquipmentService::GetSingleton().Restore(actor, snap);
			}
			break;
		case Mechanism::kEquipItem:
			REX::TRACE("[Scene] scene {:#010x} equip-item undo — removing {} item(s)", a_handle, equipItems.size());
			for (auto& [actor, item] : equipItems) {
				Equipment::EquipmentService::GetSingleton().UnequipItem(actor, item);
			}
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
			// The player free cam (MMB) is only reachable while a scene grants the input channel, so the
			// grant release is the right scope to drop it: a player-driven free cam must not outlive its
			// scene. No-op unless the player currently holds one. (Runs before the control-lock undo, so
			// the camera hands back to the still-armed third-person hold; see ForcePlayerFreeCamOff.)
			Camera::CameraService::GetSingleton().ForcePlayerFreeCamOff();
			break;
		}
	}

	void SceneRuntime::ReplayLedger(std::int32_t a_handle)
	{
		// Reverse order, once, idempotently — the cleanup guarantee. Snapshot the ledger reversed
		// (UndoMechanism erases each entry as it reverses it; a later call finds it empty and
		// no-ops). The single Fire(SCENE_END) point calls this on every termination.
		std::vector<Mechanism> reversed;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s || s->ledger.empty()) {
				return;
			}
			reversed.assign(s->ledger.rbegin(), s->ledger.rend());
		}
		for (auto m : reversed) {
			UndoMechanism(a_handle, m);
		}
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
		auto snap = Equipment::EquipmentService::GetSingleton().Hide(a_actor, a_slotMask);
		REX::DEBUG("[Scene] scene {:#010x} HideEquipment: actor {:X}, mask {:#010x}, hid {} item(s)",
			a_handle, a_actor->formID, a_slotMask, snap.stripped.size());
		if (snap.Empty()) {
			return false;
		}
		RecordHiddenEquip(a_handle, a_actor, std::move(snap));
		return true;
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

	void SceneRuntime::RecordHiddenEquip(std::int32_t a_handle, RE::Actor* a_actor, Equipment::Snapshot a_snapshot)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_handle);
		if (!s) {
			return;
		}
		s->hiddenEquip.emplace_back(a_actor, std::move(a_snapshot));
		if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kEquipment) == s->ledger.end()) {
			s->ledger.push_back(Mechanism::kEquipment);  // one ledger entry; the snapshots accumulate
		}
	}

	void SceneRuntime::RecordEquippedItem(std::int32_t a_handle, RE::Actor* a_actor, Equipment::EquippedItem a_item)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_handle);
		if (!s) {
			return;
		}
		s->equippedItems.emplace_back(a_actor, std::move(a_item));
		if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kEquipItem) == s->ledger.end()) {
			s->ledger.push_back(Mechanism::kEquipItem);  // one ledger entry; the items accumulate
		}
	}

	void SceneRuntime::RecordSheathedWeapon(std::int32_t a_handle, RE::Actor* a_actor)
	{
		std::lock_guard l{ _lock };
		Slot* s = Resolve(a_handle);
		if (!s) {
			return;
		}
		if (std::find(s->sheathedWeapon.begin(), s->sheathedWeapon.end(), a_actor) != s->sheathedWeapon.end()) {
			return;  // already recorded for this scene
		}
		s->sheathedWeapon.push_back(a_actor);
		if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kWeapon) == s->ledger.end()) {
			s->ledger.push_back(Mechanism::kWeapon);  // one ledger entry; the actors accumulate
		}
	}

	void SceneRuntime::RecordCameraState(std::int32_t a_handle, Camera::CameraMode a_mode)
	{
		bool firstForScene = false;
		std::vector<std::uint32_t> cast;
		{
			std::lock_guard l{ _lock };
			Slot* s = Resolve(a_handle);
			if (!s) {
				return;
			}
			if (a_mode == Camera::CameraMode::kSceneOrbit) {
				// Frame seed: the orbit centers/fits on the whole cast, not just the player. Form IDs, so
				// the service resolves live (post-placement) positions on the game thread at enter time.
				for (const auto* actor : s->participants) {
					if (actor) {
						cast.push_back(actor->formID);
					}
				}
			}
			if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kCameraState) == s->ledger.end()) {
				s->ledger.push_back(Mechanism::kCameraState);  // one entry; the override is released once on undo
				firstForScene = true;
			}
		}
		// Drive the service OUTSIDE _lock (it takes its own lock + posts game-thread tasks).
		auto& cam = Camera::CameraService::GetSingleton();
		if (a_mode == Camera::CameraMode::kSceneOrbit) {
			cam.SetOrbitFrameSubjects(std::move(cast));
		}
		if (firstForScene) {
			cam.AcquireStateOverride();  // capture baseline + suppress the bounce on the first holder
		}
		cam.SetLiveCameraState(a_mode);  // retarget the live camera every call (supports per-node switches)
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
			if (std::find(s->ledger.begin(), s->ledger.end(), Mechanism::kInputChannel) == s->ledger.end()) {
				s->ledger.push_back(Mechanism::kInputChannel);
			}
			grant = s->grant;
		}
		// Engage OUTSIDE _lock (the service takes its own lock + posts tasks).
		Input::InputService::GetSingleton().Engage(grant);
	}
}
