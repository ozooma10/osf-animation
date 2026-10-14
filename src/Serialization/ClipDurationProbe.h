#pragma once
#include "API/OSFEngineAPI.h"
#include <string_view>
namespace OSF::Serialization::ClipDurationProbe
{
	// Reads one source; no registry, cache persistence, threads, or game objects.
	API::ClipProbeResult Probe(std::string_view file, std::string_view animId, const API::ClipProbeResult* previous = nullptr);
}
