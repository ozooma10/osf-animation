#pragma once

#include <cstdint>

namespace OSF::Packs
{
	// GAME THREAD. Rebuild every content registry/cache and refresh browser/health projections.
	// Shared by Papyrus OSF.ReloadPacks and the Animation Browser Imports workflow.
	std::int32_t ReloadAll();
}
