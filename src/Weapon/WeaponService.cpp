#include "Weapon/WeaponService.h"

namespace OSF::Weapon
{
	namespace
	{
		// Actor::DrawWeaponMagicHands(bool) is virtual slot 0x136 on the Actor vtable
		constexpr std::size_t kDrawWeaponMagicHandsVtableSlot = 0x136;
	}

	WeaponService& WeaponService::GetSingleton()
	{
		static WeaponService instance;
		return instance;
	}

	bool WeaponService::Available()
	{
		static const bool available = []() {
			try {
				// RE::Actor::VTABLE[0] is the AddressLib id of the primary Actor vtable. 
				REL::Relocation<std::uintptr_t> vtbl{ RE::Actor::VTABLE[0] };
				const auto slot = *reinterpret_cast<const std::uintptr_t*>(
					vtbl.address() + kDrawWeaponMagicHandsVtableSlot * sizeof(std::uintptr_t));
				if (slot == 0) {
					REX::WARN("[Weapon] sheathe/draw disabled: Actor vtable slot {:#x} is null on this runtime",
						kDrawWeaponMagicHandsVtableSlot);
					return false;
				}
				REX::TRACE("[Weapon] sheathe/draw available: Actor::DrawWeaponMagicHands resolved (vtable slot {:#x} -> {:X})",
					kDrawWeaponMagicHandsVtableSlot, slot);
				return true;
			} catch (...) {
				REX::WARN("[Weapon] sheathe/draw disabled: Actor vtable binding did not resolve on this runtime");
				return false;
			}
		}();
		return available;
	}

	bool WeaponService::Sheathe(RE::Actor* a_actor)
	{
		if (!a_actor || !Available()) {
			return false;
		}
		a_actor->DrawWeaponMagicHands(false);
		REX::DEBUG("[Weapon] actor {:X}: weapon sheathed", a_actor->formID);
		return true;
	}

	void WeaponService::Draw(RE::Actor* a_actor)
	{
		if (!a_actor || !Available()) {
			return;
		}
		a_actor->DrawWeaponMagicHands(true);
		REX::DEBUG("[Weapon] actor {:X}: weapon drawn", a_actor->formID);
	}
}
