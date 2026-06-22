#pragma once

// Hides and restores an actor's worn apparel, behind the osf.equipment.hide / osf.equipment.restore actions. 
// Hide() snapshots what the actor is wearing and unequips it; Restore() re-equips the snapshot. 
// The engine's equip/unequip is synchronous and game-thread-only, so callers must be on the game thread

// The equip/unequip calls go through ActorEquipManager::{Equip,Unequip}Object;
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
	};
}
