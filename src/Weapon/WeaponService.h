#pragma once

namespace OSF::Weapon
{
	class WeaponService
	{
	public:
		static WeaponService& GetSingleton();

		// Game thread. False for a null actor.
		bool Sheathe(RE::Actor* a_actor);

		// Game thread.
		void Draw(RE::Actor* a_actor);
	};
}
