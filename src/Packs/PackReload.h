#pragma once

#include <cstdint>

namespace OSF::Content
{
	// GAME THREAD. Rebuild every content registry/cache and refresh browser/health projections.
	// Shared by the legacy Papyrus OSF.ReloadPacks entry and the Animation Browser Imports workflow.
	std::int32_t ReloadAll();
}

namespace OSF::Packs
{
	// Compatibility namespace retained for existing internal/native callers.
	inline std::int32_t ReloadAll() { return Content::ReloadAll(); }
}
