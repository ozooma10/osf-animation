#include "ActorState/Service.h"
#include "Equipment/EquipmentService.h"

#include <algorithm>

namespace OSF::ActorState
{
	Store& Get()
	{
		static Store state(Equipment::EquipmentService::GetSingleton());
		return state;
	}
	void Report(Status status, std::string_view operation)
	{
		if (status != Status::Ok && status != Status::Invalid)
			REX::WARN("[Equip] actor-state {} returned status {}; reconcile retained changes when available",
				operation, static_cast<std::uint32_t>(status));
	}
	Result Acquire(Handle scope, RE::Actor* actor, RE::TESBoundObject* object,
		Operation operation, std::uint32_t group, bool allowAdd, bool replaceGroup,
		std::optional<std::uint32_t> replaceFallbackGroup)
	{
		if (!actor || !object) return {};
		auto& state = Get();
		const auto externalBefore = state.ExternalChanges();
		const auto result = state.Acquire(scope, {actor->GetFormID(), object->GetFormID(), operation, group, allowAdd}, replaceGroup, replaceFallbackGroup);
		Report(result.status, "acquire");
		if (state.ExternalChanges() != externalBefore)
			REX::WARN("[Equip] actor {:08X}: external equipment change observed; ambiguous inventory destruction relinquished", actor->GetFormID());
		return result;
	}
	std::size_t Hide(Handle scope, RE::Actor* actor, std::uint32_t slots, bool held,
		std::span<RE::TESBoundObject* const> keep)
	{
		if (!actor || !scope || (!held && !slots)) return 0;
		auto& state = Get();
		const auto actorId = actor->GetFormID();
		const auto inventory = Equipment::EquipmentService::GetSingleton().Read(actorId);
		if (!inventory) return 0;
		const auto kept = state.Kept(actorId), hidden = state.Hidden(actorId);
		std::size_t count = 0;
		for (const auto& item : *inventory) {
			if (!item.count || item.skin || (!item.worn && std::find(hidden.begin(), hidden.end(), item.object) == hidden.end())) continue;
			if (std::find(kept.begin(), kept.end(), item.object) != kept.end()) continue;
			if (std::any_of(keep.begin(), keep.end(), [&](auto* object) { return object && object->GetFormID() == item.object; })) continue;
			const bool eligible = held ? !item.armor :
				(item.armor && (slots == 0xFFFFFFFFu || (item.slots & slots))) || (slots == 0xFFFFFFFFu && item.weapon);
			if (!eligible) continue;
			const auto result = state.Acquire(scope, {actorId, item.object, Operation::HideOptional, held ? Held : Apparel});
			Report(result.status, "hide");
			if (result) ++count;
		}
		return count;
	}
}
