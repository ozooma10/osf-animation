#pragma once

// Scene gear (docs/RFC-scene-gear.md): ARMO OSF may auto-equip for a scene, at most one per free-form slot per actor.
// Lanes: Data/OSF/**/*.osfgear.json (read-only) and <Documents>\My Games\Starfield\SFSE\OSF\scene-gear.json (user).

#include <string>
#include <vector>

namespace OSF::Equipment::Gear
{
	struct Pick
	{
		RE::TESBoundObject* object = nullptr;
		bool                worn = false;  // strip-exempt; nothing to equip or ledger
		std::string         slot;
		std::string         ref;  // "<Plugin>|0xLOCAL" as registered, for logging
	};

	// User lane first (wins ref collisions); Data files sorted, first-loaded wins.
	void LoadAll();

	// When off, SelectForActor returns nothing; registrations stay loaded.
	bool AutoEquip();
	void SetAutoEquip(bool a_enabled);

	// Game thread (resolves the form ref on first use). "" if the form isn't registered gear.
	std::string SlotOfForm(RE::TESFormID a_formId);

	// Game thread. One Pick per slot not in a_takenSlots (lowercase): override ("none" suppresses) > worn > ref order.
	std::vector<Pick> SelectForActor(RE::Actor* a_actor, const std::vector<std::string>& a_takenSlots);
}
