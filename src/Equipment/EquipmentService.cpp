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

		// The engine re-instances items while they sit unequipped (outfit/skin pass), so the live
		// inventory instanceData is the authoritative one to equip/unequip against. Returns null when
		// the object isn't in the actor's inventory (then equip uses the base instance).
		RE::TBO_InstanceData* LiveInstance(RE::Actor* a_actor, RE::TESBoundObject* a_object)
		{
			const auto guard = a_actor->inventoryList.LockRead();
			if (const RE::BGSInventoryList* list = *guard) {
				for (const auto& item : list->data) {
					if (item.object == a_object) {
						return item.instanceData.get();
					}
				}
			}
			return nullptr;
		}

		// Whether a_object is present in the actor's inventory list, and whether it's currently worn —
		// in one inventory pass. Deliberately does NOT call RE::Actor::IsObjectEquipped: that entry is
		// an unbound REL::ID(0) in this CommonLibSF (candidate AddrLib 106991, never wired), so calling
		// it aborts with "Failed to find offset for Address Library ID". item.IsEquipped() is the same
		// equipped flag Hide() reads, so this rides the already-proven strip/inventory path instead.
		struct ItemPresence
		{
			bool present = false;
			bool equipped = false;
		};
		ItemPresence FindItem(RE::Actor* a_actor, RE::TESBoundObject* a_object)
		{
			ItemPresence found;
			const auto guard = a_actor->inventoryList.LockRead();
			if (const RE::BGSInventoryList* list = *guard) {
				for (const auto& item : list->data) {
					if (item.object == a_object) {
						found.present = true;
						found.equipped = item.IsEquipped();
						break;
					}
				}
			}
			return found;
		}
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
				REX::WARN("[Equip] undress/redress disabled: ActorEquipManager singleton resolved null");
				return false;
			}
			// Make sure the two functions still look like what we expect on this game build.
			// On a mismatch we disable the feature rather than call into the wrong code.
			if (!Util::Hooking::PrologueMatches(RE::ID::ActorEquipManager::EquipObject, kExpectedPrologue) ||
				!Util::Hooking::PrologueMatches(RE::ID::ActorEquipManager::UnequipObject, kExpectedPrologue)) {
				REX::WARN("[Equip] undress/redress disabled: equip/unequip prologue mismatch on this runtime "
					"(IDs {}/{} do not match the verified bytes)",
					RE::ID::ActorEquipManager::EquipObject.id(),
					RE::ID::ActorEquipManager::UnequipObject.id());
				return false;
			}
			REX::INFO("[Equip] undress/redress available: equip/unequip prologues verified");
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
				REX::DEBUG("[Equip] actor {:X}: inventory list not materialized — nothing hidden", a_actor->formID);
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
			REX::DEBUG("[Equip] actor {:X}: hid {} worn item(s) ({} skin piece(s) excluded)", a_actor->formID, snapshot.stripped.size(), skippedSkin);
		} else {
			REX::DEBUG("[Equip] actor {:X}: hid {} worn item(s)", a_actor->formID, snapshot.stripped.size());
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
		REX::DEBUG("[Equip] actor {:X}: restored {} worn item(s)", a_actor->formID, a_snapshot.stripped.size());
	}

	EquippedItem EquipmentService::EquipItem(RE::Actor* a_actor, RE::TESBoundObject* a_object)
	{
		EquippedItem record;
		if (!a_actor || !a_object) {
			REX::DEBUG("[Equip] equip item: null {} — nothing equipped", !a_actor ? "actor" : "object");
			return record;
		}
		if (!Available()) {
			REX::WARN("[Equip] actor {:X}: cannot equip item {:X} — equip/unequip unavailable on this runtime",
				a_actor->formID, a_object->GetFormID());
			return record;
		}
		record.object = a_object;

		// Whether the actor already owns / already wears this exact form decides what we have to undo:
		// only add a copy if absent (remove it on cleanup), only equip if not already worn (unequip on
		// cleanup) — so we never disturb a copy/equip-state the actor brought into the scene. Both
		// answers come from one inventory pass (NOT RE::Actor::IsObjectEquipped — unbound ID, see FindItem).
		const ItemPresence existing = FindItem(a_actor, a_object);
		const bool hadItem = existing.present;
		const bool wasEquipped = existing.equipped;
		REX::TRACE("[Equip] actor {:X}: equipping item {:X} (hadItem={}, wasEquipped={})",
			a_actor->formID, a_object->GetFormID(), hadItem, wasEquipped);

		if (!hadItem) {
			// Transient copy (count 1), tagged kScriptAddItem like the engine's own add-if-missing branch.
			a_actor->AddObjectToContainer(a_object, {}, 1, nullptr, RE::ITEM_TRANSFER_REASON::kScriptAddItem);
			record.addedToInventory = true;
		}

		if (!wasEquipped) {
			// Re-resolve the instance AFTER the add (the engine instantiates the freshly-added copy).
			RE::BGSObjectInstance instance{ a_object, LiveInstance(a_actor, a_object) };
			// Same silent flags as Restore: queueEquip=false (immediate), forceEquip=true (skip the AI veto),
			// playSounds=false, applyNow=true, locked=false (the actor's outfit logic still owns it afterwards).
			RE::ActorEquipManager::GetSingleton()->EquipObject(a_actor, instance, nullptr, false, true, false, true, false);
			record.equipped = true;
		}

		REX::TRACE("[Equip] actor {:X}: equipped item {:X} (added={}, equipped={})",
			a_actor->formID, a_object->GetFormID(), record.addedToInventory, record.equipped);
		return record;
	}

	void EquipmentService::UnequipItem(RE::Actor* a_actor, const EquippedItem& a_item)
	{
		if (!a_actor || !a_item.object || !Available()) {
			return;
		}

		if (a_item.equipped) {
			RE::BGSObjectInstance instance{ a_item.object, LiveInstance(a_actor, a_item.object) };
			// forceUnequip=true clears any equip lock; queueUnequip=false (immediate), silent, applyNow=true.
			RE::ActorEquipManager::GetSingleton()->UnequipObject(a_actor, instance, nullptr, false, true, false, true, nullptr);
		}

		if (a_item.addedToInventory) {
			// Destroy the transient copy we added (reason kNone = destroy, inventoryIndex -1 = identify by object).
			std::uint32_t outHandle = 0;
			RE::RemoveItemRequest request;
			request.object = a_item.object;
			request.inventoryIndex = -1;
			request.count = 1;
			request.reason = RE::ITEM_TRANSFER_REASON::kNone;
			a_actor->RemoveItem(outHandle, request);
		}

		REX::TRACE("[Equip] actor {:X}: removed scene item {:X} (unequipped={}, destroyed={})",
			a_actor->formID, a_item.object->GetFormID(), a_item.equipped, a_item.addedToInventory);
	}
}
