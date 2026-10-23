#include "Equipment/EquipmentService.h"
#include "Profiling/Profile.h"
#include "RE/T/TESObjectARMO.h"
#include "Util/FormRef.h"

#include <limits>

namespace OSF::Equipment
{
	namespace {
		RE::TESBoundObject* Object(ActorState::Object id)
		{
			auto* form = RE::TESForm::LookupByID(id);
			return form && Util::IsBoundObjectType(form->GetFormType()) ? static_cast<RE::TESBoundObject*>(form) : nullptr;
		}
		ActorState::Item DescribeItem(RE::TESBoundObject* object)
		{
			ActorState::Item item;
			item.object = object->GetFormID();
			item.armor = object->IsArmor();
			item.weapon = object->IsWeapon();
			if (item.armor) {
				// Existing runtime-proven 1.16.244 ARMO slot layout; the CLSF field is still at the wrong offset.
				item.slots = *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<std::uintptr_t>(object) + 0x208);
			} else {
				// Held items share one conservative equipment channel; no hand-specific fit is inferred.
				item.slots = 1ull << 63;
			}
			return item;
		}
	}
	EquipmentService& EquipmentService::GetSingleton()
	{
		static EquipmentService service;
		return service;
	}
	std::optional<ActorState::Item> EquipmentService::Describe(ActorState::Object id)
	{
		auto* object = Object(id);
		return object ? std::optional{DescribeItem(object)} : std::nullopt;
	}
	std::optional<std::vector<ActorState::Item>> EquipmentService::Read(ActorState::Actor id)
	{
		OSF_PROFILE_SCOPE("Equip.Read");
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
		if (!actor) return {};
		const auto base = actor->GetBaseObject();
		auto* npc = base ? base->As<RE::TESNPC>() : nullptr;
		const auto* skin = npc ? npc->GetSkin() : nullptr;
		std::vector<ActorState::Item> items;
		const auto guard = actor->inventoryList.LockRead();
		const auto* list = *guard;
		if (!list) return items; // an actor's lazy inventory has no observed items yet
		for (const auto& entry : list->data) {
			if (!entry.object || !entry.object->GetFormID()) continue;
			auto item = DescribeItem(entry.object);
			std::uint64_t count = 0;
			for (const auto& stack : entry.stacks) count += stack.count;
			item.count = static_cast<std::uint32_t>(std::min<std::uint64_t>(count, std::numeric_limits<std::uint32_t>::max()));
			item.worn = entry.IsEquipped();
			item.skin = entry.object == skin;
			items.push_back(item);
		}
		return items;
	}
	bool EquipmentService::SetWorn(ActorState::Actor actorId, ActorState::Object objectId, bool worn)
	{
		OSF_PROFILE_SCOPE("Equip.SetWorn");
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
		auto* object = Object(objectId);
		auto* manager = RE::ActorEquipManager::GetSingleton();
		if (!actor || !object || !manager) return false;
		RE::BSTSmartPointer<RE::TBO_InstanceData> data;
		bool found = false;
		{
			const auto guard = actor->inventoryList.LockRead();
			if (const auto* list = *guard) for (const auto& entry : list->data) {
				if (entry.object == object) { data = entry.instanceData; found = true; break; }
			}
		}
		if (!found) return false; // never resurrect an item by restoring an old instance snapshot
		RE::BGSObjectInstance instance{object, data.get()};
		if (worn) manager->EquipObject(actor, instance, nullptr, false, true, false, true, false);
		else manager->UnequipObject(actor, instance, nullptr, false, true, false, true, nullptr);
		return true; // Store verifies the resulting inventory; this is not mesh/3D readiness.
	}
	bool EquipmentService::Add(ActorState::Actor actorId, ActorState::Object objectId)
	{
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
		auto* object = Object(objectId);
		if (!actor || !object) return false;
		actor->AddObjectToContainer(object, {}, 1, nullptr, RE::ITEM_TRANSFER_REASON::kScriptAddItem);
		return true;
	}
	bool EquipmentService::Remove(ActorState::Actor actorId, ActorState::Object objectId)
	{
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
		auto* object = Object(objectId);
		if (!actor || !object) return false;
		std::uint32_t outHandle = 0;
		RE::RemoveItemRequest request;
		request.object = object; request.inventoryIndex = -1; request.count = 1;
		request.reason = RE::ITEM_TRANSFER_REASON::kNone;
		actor->RemoveItem(outHandle, request);
		return true;
	}
}
