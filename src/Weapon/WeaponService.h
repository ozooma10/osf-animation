#pragma once

// Sheathes and re-draws an actor's weapon, behind the osf.weapon.sheathe / osf.weapon.restore actions.

namespace OSF::Weapon
{
	class WeaponService
	{
	public:
		static WeaponService& GetSingleton();

		// GAME THREAD. Holsters a_actor's weapon. Returns false for a null actor.
		bool Sheathe(RE::Actor* a_actor);

		// GAME THREAD. Un-holsters a_actor's weapon (the restore half).
		void Draw(RE::Actor* a_actor);
	};
}
