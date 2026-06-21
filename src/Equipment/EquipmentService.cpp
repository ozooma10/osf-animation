#include "Equipment/EquipmentService.h"

#include "Util/Hooking.h"

#include <array>

namespace OSF::Equipment
{
	namespace
	{
		// Equip/unequip go through ActorEquipManager::{Equip,Unequip}Object.
		//  These are the shared first 29 bytes (byte 29 differs, E0/D0 = stack frame size, so it's left out).
		constexpr std::array<std::uint8_t, 29> kExpectedPrologue{
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08,
			0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41,
			0x57, 0x48, 0x8D, 0x6C, 0x24, 0xF9, 0x48, 0x81, 0xEC
		};
	}

	EquipmentService& EquipmentService::GetSingleton()
	{
		static EquipmentService instance;
		return instance;
	}

	bool EquipmentService::Available()
	{
		static const bool available = []() {
			if (!RE::ActorEquipManager::GetSingleton()) {
				REX::WARN("Undress/redress disabled: ActorEquipManager singleton resolved null");
				return false;
			}
			// Make sure the two functions still look like what we expect on this game build.
			// On a mismatch we disable the feature rather than call into the wrong code.
			if (!Util::Hooking::PrologueMatches(RE::ID::ActorEquipManager::EquipObject, kExpectedPrologue) ||
				!Util::Hooking::PrologueMatches(RE::ID::ActorEquipManager::UnequipObject, kExpectedPrologue)) {
				REX::WARN("Undress/redress disabled: equip/unequip prologue mismatch on this runtime "
					"(IDs {}/{} do not match the verified bytes)",
					RE::ID::ActorEquipManager::EquipObject.id(),
					RE::ID::ActorEquipManager::UnequipObject.id());
				return false;
			}
			REX::INFO("Undress/redress available: equip/unequip prologues verified");
			return true;
		}();
		return available;
	}

	Snapshot EquipmentService::Hide(RE::Actor* a_actor)
	{
		Snapshot snapshot;
		if (!a_actor || !Available()) {
			return snapshot;
		}

		// Never strip the actor's base skin: on some NPCs the skin/body ARMO enumerates as an equipped inventory item, and unequipping it leaves the actor invisible. 
		// Identified by form identity (NPC override skin, else race default, via TESNPC::GetSkin) rather than the biped mask, whose offset uncertain. 
		const RE::TESObjectARMO* skin = nullptr;
		if (auto* npc = a_actor->GetNPC()) {
			skin = npc->GetSkin();
		}
		std::uint32_t skippedSkin = 0;

		{
			// Collect under the inventory read lock; mutate only after releasing it (UnequipObject takes the inventory lock inline).
			const auto guard = a_actor->inventoryList.LockRead();
			const RE::BGSInventoryList* list = *guard;
			if (!list) {
				// The list is lazy, null on NPCs whose inventory has never materialized. The player + interacted NPCs have one.
				REX::INFO("Actor {:X}: inventory list not materialized — nothing hidden", a_actor->formID);
				return snapshot;
			}
			for (const auto& item : list->data) {
				// equipped ARMO with a real form. ARMO covers apparel + spacesuit pieces;
				// weapons stay holstered.
				if (item.object && item.object->IsArmor() && item.IsEquipped() &&
					item.object->GetFormID() != 0) {
					if (skin && item.object == skin) {
						skippedSkin++;
						continue;
					}
					snapshot.stripped.push_back({ item.object, item.instanceData });
				}
			}
		}

		auto* mgr = RE::ActorEquipManager::GetSingleton();
		for (const auto& w : snapshot.stripped) {
			RE::BGSObjectInstance instance{ w.object, w.instanceData.get() };
			// Verified silent-strip flags: queueUnequip=false (immediate, not via the AI-tick list), forceUnequip=true (unequips despite any lock and clears it), 
			// playSounds= false (fully silent), applyNow=true, slotBeingReplaced=nullptr.
			mgr->UnequipObject(a_actor, instance, nullptr, false, true, false, true, nullptr);
		}
		if (skippedSkin > 0) {
			REX::INFO("Actor {:X}: hid {} worn item(s) ({} skin piece(s) excluded)", a_actor->formID, snapshot.stripped.size(), skippedSkin);
		} else {
			REX::INFO("Actor {:X}: hid {} worn item(s)", a_actor->formID, snapshot.stripped.size());
		}
		return snapshot;
	}

	void EquipmentService::Restore(RE::Actor* a_actor, const Snapshot& a_snapshot)
	{
		if (!a_actor || a_snapshot.Empty() || !Available()) {
			return;
		}

		auto* mgr = RE::ActorEquipManager::GetSingleton();
		for (const auto& w : a_snapshot.stripped) {
			if (!w.object) {
				continue;
			}
			// Re-resolve instanceData from the live item: the engine's outfit/skin pass can re-instance items while unequipped, so the snapshot's instanceData may be stale.
			// Fall back to the snapshot copy.
			RE::TBO_InstanceData* liveInstance = w.instanceData.get();
			{
				const auto guard = a_actor->inventoryList.LockRead();
				if (const RE::BGSInventoryList* list = *guard) {
					for (const auto& item : list->data) {
						if (item.object == w.object) {
							liveInstance = item.instanceData.get();
							break;
						}
					}
				}
			}
			RE::BGSObjectInstance instance{ w.object, liveInstance };
			// locked=false so the actor's own outfit logic owns the items again after the scene; forceEquip=true skips the AI veto, silent.
			mgr->EquipObject(a_actor, instance, nullptr, false, true, false, true, false);
		}
		REX::INFO("Actor {:X}: restored {} worn item(s)", a_actor->formID, a_snapshot.stripped.size());
	}
}
