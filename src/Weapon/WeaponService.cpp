#include "Weapon/WeaponService.h"

namespace OSF::Weapon
{
	WeaponService& WeaponService::GetSingleton()
	{
		static WeaponService instance;
		return instance;
	}

	bool WeaponService::Sheathe(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		a_actor->DrawWeaponMagicHands(false);
		REX::DEBUG("[Weapon] actor {:X}: weapon sheathed", a_actor->formID);
		return true;
	}

	void WeaponService::Draw(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		a_actor->DrawWeaponMagicHands(true);
		REX::DEBUG("[Weapon] actor {:X}: weapon drawn", a_actor->formID);
	}
}
