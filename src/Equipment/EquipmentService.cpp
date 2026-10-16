#include "Equipment/EquipmentService.h"

#include "RE/T/TESObjectARMO.h"

#include <algorithm>

namespace OSF::Equipment
{
	namespace
	{
		RE::TESNPC* NpcOf(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return nullptr;
			}
			const auto base = a_actor->GetBaseObject();
			return base ? base->As<RE::TESNPC>() : nullptr;
		}

		// The engine re-instances unequipped items (outfit/skin pass); the live instanceData is authoritative. Null if absent.
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

		// Not RE::Actor::IsObjectEquipped: unbound REL::ID(0) in this CommonLibSF (crashes).
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

	Snapshot EquipmentService::Hide(RE::Actor* a_actor)
	{
		return Hide(a_actor, 0xFFFFFFFFu);
	}

	Snapshot EquipmentService::Hide(RE::Actor* a_actor, std::uint32_t a_slotMask, std::span<RE::TESBoundObject* const> a_keep)
	{
		Snapshot snapshot;
		if (!a_actor || a_slotMask == 0) {
			return snapshot;
		}
		auto* mgr = RE::ActorEquipManager::GetSingleton();
		if (!mgr) {
			return snapshot;
		}

		// Some NPCs enumerate the skin ARMO as equipped; unequipping it makes them invisible. Matched by form identity.
		const RE::TESObjectARMO* skin = nullptr;
		if (auto* npc = NpcOf(a_actor)) {
			skin = npc->GetSkin();
		}
		std::uint32_t skippedSkin = 0;

		{
			// Mutate only after releasing the read lock; UnequipObject takes the inventory lock.
			const auto guard = a_actor->inventoryList.LockRead();
			const RE::BGSInventoryList* list = *guard;
			if (!list) {
				// Lazy list, null on NPCs whose inventory never materialized.
				REX::DEBUG("[Equip] actor {:X}: inventory list not materialized — nothing hidden", a_actor->formID);
				return snapshot;
			}
			const bool fullStrip = (a_slotMask == 0xFFFFFFFFu);
			for (const auto& item : list->data) {
				if (!item.object || !item.IsEquipped() || item.object->GetFormID() == 0) {
					continue;
				}
				const bool isArmor = item.object->IsArmor();
				const bool isWeapon = fullStrip && item.object->IsWeapon();
				if (!isArmor && !isWeapon) {
					continue;
				}
				// Carried scene-owned items include weapons as well as apparel.
				if (std::find(a_keep.begin(), a_keep.end(), item.object) != a_keep.end()) continue;
				if (isArmor) {
					if (skin && item.object == skin) {
						skippedSkin++;
						continue;
					}
					if (!fullStrip) {
						// Biped slot mask: u64 at ARMO+0x208 on 1.16.244 (runtime-proven). CLSF's header (0x230, u32) is wrong for this build.
						const auto bipedSlots = *reinterpret_cast<const std::uint64_t*>(
							reinterpret_cast<std::uintptr_t>(item.object) + 0x208);
						if ((bipedSlots & a_slotMask) == 0) {
							continue;
						}
					}
				}
				snapshot.hiddenItems.push_back({ item.object, item.instanceData });
			}
		}

		for (const auto& w : snapshot.hiddenItems) {
			RE::BGSObjectInstance instance{ w.object, w.instanceData.get() };
			mgr->UnequipObject(a_actor, instance, nullptr, false, true, false, false, nullptr);
		}
		
		REX::DEBUG("[Equip] actor {:X}: hid {} worn item(s)", a_actor->formID, snapshot.hiddenItems.size());
		return snapshot;
	}

	Snapshot EquipmentService::HideHeld(RE::Actor* a_actor, std::span<RE::TESBoundObject* const> a_keep)
	{
		Snapshot snapshot;
		if (!a_actor) {
			return snapshot;
		}
		auto* mgr = RE::ActorEquipManager::GetSingleton();
		if (!mgr) {
			return snapshot;
		}

		{
			const auto guard = a_actor->inventoryList.LockRead();
			const RE::BGSInventoryList* list = *guard;
			if (!list) {
				return snapshot;
			}
			for (const auto& item : list->data) {
				if (!item.object || !item.IsEquipped() || item.object->GetFormID() == 0) {
					continue;
				}
				// ARMO is Hide()'s business; this also covers the skin.
				if (item.object->IsArmor()) {
					continue;
				}
				if (std::find(a_keep.begin(), a_keep.end(), item.object) != a_keep.end()) {
					continue;
				}
				snapshot.hiddenItems.push_back({ item.object, item.instanceData });
			}
		}

		for (const auto& w : snapshot.hiddenItems) {
			RE::BGSObjectInstance instance{ w.object, w.instanceData.get() };
			mgr->UnequipObject(a_actor, instance, nullptr, false, true, false, false, nullptr);
		}

		REX::DEBUG("[Equip] actor {:X}: cleared {} held item(s)", a_actor->formID, snapshot.hiddenItems.size());
		return snapshot;
	}

	void EquipmentService::Restore(RE::Actor* a_actor, const Snapshot& a_snapshot)
	{
		if (!a_actor || a_snapshot.Empty()) {
			return;
		}

		auto* mgr = RE::ActorEquipManager::GetSingleton();
		if (!mgr) {
			return;
		}
		for (const auto& w : a_snapshot.hiddenItems) {
			if (!w.object) {
				continue;
			}
			// The outfit/skin pass may have re-instanced the item; fall back to the snapshot copy.
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
			// locked=false so the actor's outfit logic owns the items again; forceEquip=true skips the AI veto.
			mgr->EquipObject(a_actor, instance, nullptr, false, true, false, true, false);
		}
		REX::DEBUG("[Equip] actor {:X}: restored {} worn item(s)", a_actor->formID, a_snapshot.hiddenItems.size());
	}

	EquippedItem EquipmentService::EquipItem(RE::Actor* a_actor, RE::TESBoundObject* a_object)
	{
		EquippedItem record;
		if (!a_actor || !a_object) {
			REX::DEBUG("[Equip] equip item: null {} — nothing equipped", !a_actor ? "actor" : "object");
			return record;
		}
		auto* mgr = RE::ActorEquipManager::GetSingleton();
		if (!mgr) {
			return record;
		}
		record.object = a_object;

		// Undo only what we change: add only if absent, equip only if not worn.
		const ItemPresence existing = FindItem(a_actor, a_object);
		const bool hadItem = existing.present;
		const bool wasEquipped = existing.equipped;
		REX::TRACE("[Equip] actor {:X}: equipping item {:X} (hadItem={}, wasEquipped={})",
			a_actor->formID, a_object->GetFormID(), hadItem, wasEquipped);

		if (!hadItem) {
			// kScriptAddItem, like the engine's own add-if-missing branch.
			a_actor->AddObjectToContainer(a_object, {}, 1, nullptr, RE::ITEM_TRANSFER_REASON::kScriptAddItem);
			record.addedToInventory = true;
		}

		if (!wasEquipped) {
			// After the add: the engine instantiates the fresh copy.
			RE::BGSObjectInstance instance{ a_object, LiveInstance(a_actor, a_object) };
			// Silent flags as in Restore.
			mgr->EquipObject(a_actor, instance, nullptr, false, true, false, true, false);
			record.equipped = true;
		}

		REX::TRACE("[Equip] actor {:X}: equipped item {:X} (added={}, equipped={})",
			a_actor->formID, a_object->GetFormID(), record.addedToInventory, record.equipped);
		return record;
	}

	void EquipmentService::UnequipItem(RE::Actor* a_actor, const EquippedItem& a_item)
	{
		if (!a_actor || !a_item.object) {
			return;
		}

		auto* mgr = RE::ActorEquipManager::GetSingleton();
		if (!mgr) {
			return;
		}
		if (a_item.equipped) {
			RE::BGSObjectInstance instance{ a_item.object, LiveInstance(a_actor, a_item.object) };
			// forceUnequip=true clears any equip lock.
			mgr->UnequipObject(a_actor, instance, nullptr, false, true, false, true, nullptr);
		}

		if (a_item.addedToInventory) {
			// reason kNone = destroy; inventoryIndex -1 = match by object.
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
