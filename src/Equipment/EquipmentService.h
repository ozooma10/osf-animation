#pragma once

// Manages an actor's worn equipment for a scene. Two independent features:
//   - Hide()/Restore(): snapshot + strip the actor's worn apparel, then re-equip it (osf.equipment.hide / .restore).
//   - EquipItem()/UnequipItem(): equip an ARBITRARY item for the scene's duration, then take it back off
//     (osf.equipment.equip / .unequip). If the actor didn't already own the item we add a transient copy and
//     destroy it on cleanup, so an authored prop/outfit leaves no trace in the inventory after the scene.
// The engine's equip/unequip is synchronous and game-thread-only, so callers must be on the game thread.

// The equip/unequip calls go through ActorEquipManager::{Equip,Unequip}Object; add/remove go through
// TESObjectREFR::{AddObjectToContainer,RemoveItem} (vtable calls, always bound).
// A couple of things we leave to in-game behaviour rather than guess at:
//   - the base skin/body ARMO is left on, matched by form identity (TESNPC::GetSkin) rather than the biped mask, so we can never accidentally make an actor invisible;
//   - an NPC whose inventory list has never been built simply isn't stripped, the player and any NPC you've interacted with always have one.

namespace OSF::Equipment
{
	// worn inventory entry captured at strip time. The form pointer is stable for the session; 
	// instanceData is refcounted so the snapshot keeps any mod/instance state alive across the scene.
	struct WornItem
	{
		RE::TESBoundObject*                       object = nullptr;
		RE::BSTSmartPointer<RE::TBO_InstanceData> instanceData;
	};

	struct Snapshot
	{
		std::vector<WornItem> stripped;

		[[nodiscard]] bool Empty() const noexcept { return stripped.empty(); }
	};

	// One arbitrary item EquipItem() put on an actor, with exactly what it has to undo on cleanup:
	// unequip only if WE equipped it (don't strip a copy the actor already wore), and destroy the
	// added copy only if WE added it (don't take away the actor's own).
	struct EquippedItem
	{
		RE::TESBoundObject* object = nullptr;        // null => EquipItem failed / unavailable (nothing to undo)
		bool                addedToInventory = false;  // we added a transient copy — remove it on cleanup
		bool                equipped = false;          // we equipped it (wasn't already worn) — unequip on cleanup
	};

	class EquipmentService
	{
	public:
		static EquipmentService& GetSingleton();

		bool Available();

		// GAME THREAD. Snapshots + unequips the actor's worn apparel (all equipped ARMO except the base skin).
		// Empty snapshot = nothing hidden (nothing to restore later).
		Snapshot Hide(RE::Actor* a_actor);

		// GAME THREAD. Re-equips the apparel recorded in a_snapshot (idempotent, re-equipping an item the actor already wears is a no-op).
		void Restore(RE::Actor* a_actor, const Snapshot& a_snapshot);

		// GAME THREAD. Equips a_object on a_actor for the scene's duration. Adds a transient copy if the
		// actor doesn't already own one. Returns the record describing what was done (object null = failed
		// / unavailable); pass it to UnequipItem to reverse exactly that (and nothing the actor already had).
		EquippedItem EquipItem(RE::Actor* a_actor, RE::TESBoundObject* a_object);

		// GAME THREAD. Reverses EquipItem: unequips the item if we equipped it, then destroys the added
		// copy if we added it. No-op for a default/failed record.
		void UnequipItem(RE::Actor* a_actor, const EquippedItem& a_item);
	};
}
