#pragma once

// User-side scene gear (docs/RFC-scene-gear.md): a registry of ARMO forms OSF may auto-equip on a
// scene participant for the scene's duration, grouped by free-form exclusivity "slots" (at most one
// item per slot per actor per scene). Two lanes:
//   - Data/OSF/**/*.osfgear.json — shipped by gear mods / hand-written, treated as read-only;
//   - <Documents>\My Games\Starfield\SFSE\OSF\scene-gear.json — the user lane (registrations +
//     per-actor overrides; hand-editable in v1, the cast-manager UI writes it in v1.5).
// The inventory is the per-actor configuration surface: a registered item an actor carries is a
// candidate; SelectForActor picks at most one per slot (override > worn > stable ref order).
// Content-neutral Layer-C: no scene knowledge — SceneRuntime decides when to strip-exempt/equip.

#include <string>
#include <vector>

namespace OSF::Equipment::Gear
{
	// One selected item for one actor — what the scene-start gear pass should do about it.
	struct Pick
	{
		RE::TESBoundObject* object = nullptr;
		bool                worn = false;  // already worn at selection: strip-exempt it; nothing to equip or ledger
		std::string         slot;
		std::string         ref;  // "<Plugin>|0xLOCAL" as registered, for logging
	};

	// Scan both lanes (user lane first — it wins ref collisions; Data files in sorted order,
	// first-loaded wins, matching the scene registry). Runs at kPostDataLoad and on OSF.ReloadPacks().
	void LoadAll();

	// Mirror of the `gear.autoEquip` user setting (default ON; UISettings applies commits).
	// When off, SelectForActor returns nothing — registration/overrides stay loaded.
	bool AutoEquip();
	void SetAutoEquip(bool a_enabled);

	// The registered slot of a form, or "" if the form isn't registered gear. GAME THREAD
	// (first use resolves the entry's form ref). SceneRuntime uses this to give a scene's
	// authored role `equip` precedence over the gear pass for the slot it occupies.
	std::string SlotOfForm(RE::TESFormID a_formId);

	// GAME THREAD. The actor's gear selection: for each registered slot with at least one candidate
	// in the actor's inventory (skipping a_takenSlots, lowercase), exactly one Pick — the per-actor
	// override first ("none" suppresses the slot; an override naming an item the actor doesn't carry
	// is ignored), else a worn candidate, else the first by ref order. Empty when auto-equip is off,
	// the actor has no materialized inventory list, or nothing matches.
	std::vector<Pick> SelectForActor(RE::Actor* a_actor, const std::vector<std::string>& a_takenSlots);
}
