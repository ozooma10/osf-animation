#pragma once

// Animation-wheel loadout in SFSE\OSF\wheel-pins.json. File absent = derive from installed
// `player.emote.*` entries; [] = intentionally empty. Stale entries are kept so reinstalled packs revive.
// Leaf lock: safe under the registry read lock; must never call back into the registry.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace OSF::Serialization::WheelPins
{
	struct Entry
	{
		std::string  scene;
		std::int32_t stage = -1;  // -1 = scene entry

		bool operator==(const Entry&) const = default;
	};

	bool Customized();

	// Empty when defaults are active.
	std::vector<Entry> Entries();

	// Dedupes, drops empty entries, caps at 12; persists on change.
	bool SetEntries(std::span<const Entry> a_entries);

	bool Reset();
}
