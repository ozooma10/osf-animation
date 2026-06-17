#pragma once

// Scene undress/redress mechanism for the osf.equipment.hide / osf.equipment.restore
// actions. Hide() snapshots an actor's worn apparel and unequips it; Restore() re-equips
// the snapshot. Engine equip/unequip is synchronous + game-thread-only (RE-verified), so
// callers must invoke from the game thread (SceneRuntime's action dispatch already runs there).
//
// Equip path RE-verified (osf-re gameplay.actor_equipment module): the
// ActorEquipManager::{Equip,Unequip}Object 8/9-arg ABI, silent-strip flag semantics, and
// game-thread affinity all confirmed. CLSF maps the IDs (ID::ActorEquipManager::{Equip,
// Unequip}Object = 101949/101951); Available() still prologue-gates them (the RE was
// disassembled on 1.16.236, the byte check confirms the same code on 1.16.244) and the
// feature self-disables on a mismatch.
//
// Lean carve: this is hide/restore only. The pre-split EquipmentService also added scene-only
// ARMO forms (the adult-content "scene equipment" path) — that machinery (ResolveArmor,
// add-then-equip, native inventory remove, the Papyrus removal fallback) is NOT part of the
// content-neutral hide/restore action and is left in the archive.
//
// Caveats inherited from the RE (deferred to in-game observation, not guessed offsets):
//   - the base skin/body ARMO is excluded from the strip by FORM identity (TESNPC::GetSkin),
//     not the contested biped mask, so an invisible actor can't result;
//   - an NPC whose lazy inventory list has never materialized isn't stripped (no crash) —
//     the player + interacted NPCs always have a list.

namespace OSF::Equipment
{
	// One worn inventory entry captured at strip time. The form pointer is stable for the
	// session; instanceData is refcounted so the snapshot keeps any mod/instance state alive
	// across the scene.
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

		// False until the equip/unequip IDs are mapped AND byte-verified on this runtime.
		// Result cached; the disabled state logs once.
		bool Available();

		// User-settings toggle (§1.5): when false the osf.equipment.hide action silent-skips.
		// Cleanup restore always runs regardless (never strand an actor undressed).
		void SetEnabled(bool a_enabled) { enabled.store(a_enabled, std::memory_order_relaxed); }
		bool Enabled() const { return enabled.load(std::memory_order_relaxed); }

		// GAME THREAD. Snapshots + unequips the actor's worn apparel (all equipped ARMO except
		// the base skin). Empty snapshot = nothing hidden (nothing to restore later).
		Snapshot Hide(RE::Actor* a_actor);

		// GAME THREAD. Re-equips the apparel recorded in a_snapshot (idempotent — re-equipping
		// an item the actor already wears is a no-op).
		void Restore(RE::Actor* a_actor, const Snapshot& a_snapshot);

	private:
		std::atomic<bool> enabled{ true };
	};
}
