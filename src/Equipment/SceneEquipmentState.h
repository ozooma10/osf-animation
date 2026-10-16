#pragma once

#include "Equipment/EquipmentService.h"
#include <algorithm>
#include <ranges>

namespace OSF::Equipment
{
	using HiddenState = std::vector<std::pair<RE::Actor*, Snapshot>>;
	using EquippedState = std::vector<std::pair<RE::Actor*, EquippedItem>>;

	// Keep the first pre-scene state of each actor/item. Subsequent snapshots contribute only
	// items hidden for the first time; cleanup reverses these increments newest first.
	inline void AppendHidden(HiddenState& state, RE::Actor* actor, Snapshot snapshot, const HiddenState& other = {})
	{
		Snapshot increment;
		for (auto& item : snapshot.hiddenItems) {
			const auto contains = [&](const auto& row) {
				return row.first == actor && std::ranges::any_of(row.second.hiddenItems,
					[&](const auto& old) { return old.object == item.object; });
			};
			const bool recorded = std::ranges::any_of(state, contains) || std::ranges::any_of(other, contains);
			if (item.object && !recorded && !std::ranges::any_of(increment.hiddenItems,
				[&](const auto& old) { return old.object == item.object; })) increment.hiddenItems.push_back(std::move(item));
		}
		if (!increment.Empty()) state.emplace_back(actor, std::move(increment));
	}

	inline bool OwnsItem(const EquippedState& state, RE::Actor* actor, RE::TESBoundObject* object)
	{
		return std::ranges::any_of(state, [&](const auto& row) { return row.first == actor && row.second.object == object; });
	}

	template <class State, class Undo>
	void UndoNewestFirst(State& state, Undo undo)
	{
		for (auto& [actor, value] : state | std::views::reverse) undo(actor, value);
	}
}
