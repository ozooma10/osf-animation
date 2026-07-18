#pragma once

// Persistent ordered emote-wheel loadout. Before the user customizes it, the view
// derives the wheel from installed `player.emote.*` entries. The first add/remove
// materializes that complete default list and persists the edited result here.
//
// Lives in <Documents>\My Games\Starfield\OSF\wheel-pins.json as a plain ordered JSON
// array. File absent means installed defaults; [] means an intentionally empty wheel.
// Stale ids (their pack uninstalled) are kept, never pruned — they revive on reinstall;
// filtering happens naturally because the catalog only stamps pinned on scenes that exist.
//
// Leaf lock: safe to call under the registry read lock (like ClipDurations::Lookup);
// must never call back into the registry.

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::Serialization::WheelPins
{
	// Whether an explicit loadout exists. False means "derive installed defaults";
	// true with zero entries is an intentionally empty wheel.
	bool Customized();

	// 1-based position in the explicit loadout, 0 = absent or not customized.
	int Order(std::string_view a_sceneId);

	// Replace the explicit ordered loadout. Duplicate/empty ids are removed while
	// preserving first occurrence. Persists on change; at most 12 entries.
	bool SetEntries(std::span<const std::string> a_sceneIds);

	// Delete the explicit loadout so installed player.emote.* entries become the wheel again.
	bool Reset();
}
