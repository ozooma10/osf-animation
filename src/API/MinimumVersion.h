#pragma once

#include "API/OSFSceneAPI.h"

namespace OSF::API::MinimumVersion
{
	// Record one consumer's minimum supported OSF Animation version. Safe from
	// any thread and before kPostDataLoad; early reports are retained until the
	// HUD and OSF UI health bridge are available.
	MinimumVersionResult Report(
		const char* a_consumer, std::uint32_t a_major,
		std::uint32_t a_minor, std::uint32_t a_patch);

	// Allow player-facing warnings and flush requirements reported during plugin
	// load. Called once at kPostDataLoad after System Health connects.
	void EnablePrompts();
}
