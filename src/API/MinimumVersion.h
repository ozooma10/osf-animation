#pragma once

#include "API/OSFSceneAPI.h"

namespace OSF::API::MinimumVersion
{
	// Thread-safe and callable before kPostDataLoad; early reports are held until EnablePrompts.
	MinimumVersionResult Report(
		const char* a_consumer, std::uint32_t a_major,
		std::uint32_t a_minor, std::uint32_t a_patch);

	// Manifest path: separate dedup id and player-facing name (Report uses its label for both).
	MinimumVersionResult ReportForConsumer(
		const char* a_consumerId, const char* a_consumerName,
		std::uint32_t a_major, std::uint32_t a_minor, std::uint32_t a_patch);

	// Called once at kPostDataLoad, after Health connects; shows held warnings.
	void EnablePrompts();
}
