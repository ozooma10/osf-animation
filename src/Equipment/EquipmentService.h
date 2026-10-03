#pragma once

// Scene-scoped worn-equipment changes. Equip/unequip is synchronous and game-thread-only.
// EquipItem adds a transient copy when the actor doesn't own the item and removes it on cleanup.
// The skin ARMO (TESNPC::GetSkin) is matched by form identity and never stripped, so an actor can't go invisible.
// An NPC whose inventory list was never built isn't stripped.

namespace OSF::Equipment
{
	// `object` stays valid only while the item remains in the actor's inventory (forms are refcounted).
	struct WornItem
	{
		RE::TESBoundObject*                       object = nullptr;
		RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
	};

	struct Snapshot
	{
		std::vector<WornItem> hiddenItems;

		[[nodiscard]] bool Empty() const noexcept { return hiddenItems.empty(); }
	};

	// Undo record: unequip only if we equipped it, remove only the copy we added.
	struct EquippedItem
	{
		RE::TESBoundObject* object = nullptr;        // null = nothing to undo
		bool                addedToInventory = false;
		bool                equipped = false;
	};

	class EquipmentService
	{
	public:
		static EquipmentService& GetSingleton();

		// Game thread. Hides worn apparel including the weapon; empty snapshot = nothing to restore.
		Snapshot Hide(RE::Actor* a_actor);

		// Game thread. Only ARMO overlapping a_slotMask (0 = none, 0xFFFFFFFF = all, plus weapon).
		// a_keep stays worn and out of the snapshot (scene-gear exemption).
		Snapshot Hide(RE::Actor* a_actor, std::uint32_t a_slotMask, std::span<RE::TESBoundObject* const> a_keep = {});

		// Game thread. Strips held (non-ARMO) items, which otherwise stay welded to the hand. a_keep as in Hide().
		Snapshot HideHeld(RE::Actor* a_actor, std::span<RE::TESBoundObject* const> a_keep = {});

		// Game thread.
		void Restore(RE::Actor* a_actor, const Snapshot& a_snapshot);

		// Game thread. Pass the result to UnequipItem to reverse exactly what was done.
		EquippedItem EquipItem(RE::Actor* a_actor, RE::TESBoundObject* a_object);

		// Game thread. No-op for a default/failed record.
		void UnequipItem(RE::Actor* a_actor, const EquippedItem& a_item);
	};
}
