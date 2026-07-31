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

	// Manifest path: use a stable machine id for deduplication and a separate
	// player-facing name. The native/Papyrus surface above uses its label as both.
	MinimumVersionResult ReportForConsumer(
		const char* a_consumerId, const char* a_consumerName,
		std::uint32_t a_major, std::uint32_t a_minor, std::uint32_t a_patch);

	// Allow player-facing warnings and flush requirements reported during plugin
	// load. Called once at kPostDataLoad after System Health connects.
	void EnablePrompts();
}
