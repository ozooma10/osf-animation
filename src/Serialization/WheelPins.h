#pragma once

// Persistent ordered animation-wheel loadout. Before the user customizes it, the view
// derives the wheel from installed `player.emote.*` entries. The first add/remove
// materializes that complete default list and persists the edited result here.
//
// Lives in <Documents>\My Games\Starfield\OSF\wheel-pins.json as an ordered JSON array
// of minimal {scene, stage?} launch references. File absent means installed defaults;
// [] means an intentionally empty wheel. Stale entries are kept so they revive if their
// animation pack is reinstalled.
//
// Leaf lock: safe to call under the registry read lock (like ClipDurations::Lookup);
// must never call back into the registry.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace OSF::Serialization::WheelPins
{
	struct Entry
	{
		std::string  scene;
		std::int32_t stage = -1;  // -1 = launch the scene entry; >= 0 = enter that animation stage

		bool operator==(const Entry&) const = default;
	};

	// Whether an explicit loadout exists. False means "derive installed defaults";
	// true with zero entries is an intentionally empty wheel.
	bool Customized();

	// Copy the explicit ordered loadout. Empty when defaults are active.
	std::vector<Entry> Entries();

	// Replace the explicit ordered loadout. Duplicate/empty entries are removed while
	// preserving first occurrence. Persists on change; at most 12 entries.
	bool SetEntries(std::span<const Entry> a_entries);

	// Delete the explicit loadout so installed player.emote.* defaults become the wheel again.
	bool Reset();
}
