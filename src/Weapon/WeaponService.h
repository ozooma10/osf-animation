#pragma once

// Sheathes and re-draws an actor's weapon, behind the osf.weapon.sheathe /
// osf.weapon.restore actions. Sheathe() holsters; Draw() un-holsters. Both go through
// the engine's Actor::DrawWeaponMagicHands(bool) virtual (CLSF Actor vtable slot 0x136),
// the same call the vanilla weapon-toggle uses. The call is synchronous and game-thread-
// only, so callers must be on the game thread (the scene runtime's action dispatch is).
//
// This is a content-neutral mechanism: it only toggles weapon-drawn state, it has no scene
// knowledge. Available() resolves the Actor vtable binding once and disables the feature if
// it doesn't resolve on this game build (verify-before-call, like EquipmentService).
//
// Restore semantics (v1): sheathe/restore are a SYMMETRIC pair, mirroring osf.control.lock —
// the scene runtime records the actors it sheathed in the per-handle undo ledger and re-draws
// them on any termination. It is therefore author-driven: only sheathe a role you know is
// armed, exactly as you'd only osf.equipment.hide an actor you mean to strip. A state-aware
// restore (skip re-drawing an actor that had nothing drawn) needs the actorState weapon-drawn
// bit, which isn't verified on this build yet — tracked in docs/RE.md as a future refinement.

namespace OSF::Weapon
{
	class WeaponService
	{
	public:
		static WeaponService& GetSingleton();

		// False until the Actor::DrawWeaponMagicHands vtable binding resolves on this runtime.
		// Result cached; the disabled state logs once.
		bool Available();

		// The user's weapon toggle: when false, osf.weapon.sheathe quietly does nothing.
		// Draw always runs regardless, so we never leave an actor stuck holstered by a scene.
		void SetEnabled(bool a_enabled) { enabled.store(a_enabled, std::memory_order_relaxed); }
		bool Enabled() const { return enabled.load(std::memory_order_relaxed); }

		// GAME THREAD. Holsters a_actor's weapon. Returns false if the actor is null or the
		// feature is unavailable (nothing recorded for restore in that case).
		bool Sheathe(RE::Actor* a_actor);

		// GAME THREAD. Un-holsters a_actor's weapon (the restore half; runs regardless of the
		// user toggle so a scene never strands an actor holstered).
		void Draw(RE::Actor* a_actor);

	private:
		std::atomic<bool> enabled{ true };
	};
}
