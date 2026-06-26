#pragma once

// Sheathes and re-draws an actor's weapon, behind the osf.weapon.sheathe / osf.weapon.restore actions.

namespace OSF::Weapon
{
	class WeaponService
	{
	public:
		static WeaponService& GetSingleton();

		// False until the Actor::DrawWeaponMagicHands vtable binding resolves on this runtime.
		// Result cached; the disabled state logs once.
		bool Available();

		// GAME THREAD. Holsters a_actor's weapon. Returns false if the actor is null or the feature is unavailable (nothing recorded for restore in that case).
		bool Sheathe(RE::Actor* a_actor);

		// GAME THREAD. Un-holsters a_actor's weapon (the restore half).
		void Draw(RE::Actor* a_actor);
	};
}
