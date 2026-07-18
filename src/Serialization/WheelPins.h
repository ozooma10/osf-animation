#pragma once

// Persistent ordered list of scene ids the user pinned to the emote wheel from the
// scene browser. When any pins exist the wheel shows exactly them, in pin order
// (pool logic is view-side); with none, the wheel falls back to the tag-prefix pool.
//
// Lives in <Documents>\My Games\Starfield\OSF\wheel-pins.json. Stale ids (their pack
// uninstalled) are kept, never pruned — they revive on reinstall; filtering happens
// naturally because the catalog only stamps `pinned` on scenes that exist.
//
// Leaf lock: safe to call under the registry read lock (like ClipDurations::Lookup);
// must never call back into the registry.

#include <string_view>

namespace OSF::Serialization::WheelPins
{
	// 1-based pin position of the scene on the wheel, 0 = not pinned.
	int Order(std::string_view a_sceneId);

	// Pin (append at the end) or unpin (later pins shift up). Persists on change.
	// Returns true when the list actually changed.
	bool Set(std::string_view a_sceneId, bool a_pinned);
}
