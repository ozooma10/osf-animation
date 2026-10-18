#include "Scene/SceneRuntime.h"

#include "Equipment/SceneEquipmentState.h"

namespace OSF::Scene
{
	void SceneRuntime::OnSaveBegin()
	{
		SetEquipmentSaveWindow(true);
	}

	void SceneRuntime::OnSaveEnd()
	{
		SetEquipmentSaveWindow(false);
	}

	void SceneRuntime::SetEquipmentSaveWindow(bool a_saving)
	{
		struct EquipmentView
		{
			std::vector<Mechanism> ledger;
			Equipment::HiddenState apparel;
			Equipment::HiddenState held;
			Equipment::EquippedState items;
		};
		std::vector<EquipmentView> scenes;
		{
			std::lock_guard l{ _lock };
			for (auto& slot : _slots) {
				if (!slot.generation || slot.ended || slot.equipmentSuspended == a_saving) continue;
				slot.equipmentSuspended = a_saving;
				scenes.push_back({ slot.ledger, slot.hiddenEquip, slot.hiddenHeldEquip, slot.equippedItems });
			}
		}

		// The existing save lifecycle runs on the game thread. Inventory operations stay outside
		// _lock. Keep the original ledger intact so an eventual END still reverses exactly once.
		auto& equipment = Equipment::EquipmentService::GetSingleton();
		for (auto& scene : scenes) {
			if (a_saving) {
				for (auto mechanism : scene.ledger | std::views::reverse) {
					if (mechanism == Mechanism::kEquipItem) {
						Equipment::UndoNewestFirst(scene.items, [&](auto* actor, const auto& item) {
							equipment.UnequipItem(actor, item);
						});
					} else if (mechanism == Mechanism::kEquipment || mechanism == Mechanism::kHeldEquipment) {
						auto& hidden = mechanism == Mechanism::kEquipment ? scene.apparel : scene.held;
						Equipment::UndoNewestFirst(hidden, [&](auto* actor, const auto& snapshot) {
							equipment.Restore(actor, snapshot);
						});
					}
				}
			} else {
				// Only still-live, previously suspended slots arrive here. Clear/load or END must never
				// reapply equipment to released actors, nor may a new scene inherit an old save window.
				for (auto mechanism : scene.ledger) {
					if (mechanism == Mechanism::kEquipItem) {
						for (const auto& [actor, item] : scene.items) {
							if (item.equipped) equipment.EquipItem(actor, item.object);
						}
					} else if (mechanism == Mechanism::kEquipment || mechanism == Mechanism::kHeldEquipment) {
						const auto& hidden = mechanism == Mechanism::kEquipment ? scene.apparel : scene.held;
						for (const auto& [actor, snapshot] : hidden) equipment.Rehide(actor, snapshot);
					}
				}
			}
		}
	}
}
