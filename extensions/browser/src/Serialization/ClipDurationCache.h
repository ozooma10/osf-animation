#pragma once
#include "OSFCatalog.h"
#include <functional>
#include <optional>
#include <string_view>

namespace OSF::Browser::ClipDurationCache
{
	// Browser-owned enrichment. The public catalog provides canonical keys; no private engine includes.
	std::optional<float> Lookup(std::string_view key);
	// Memory-only merge of newly decoded engine durations; safe during a scan.
	void RefreshDecoded();
	// Copy references on the game thread. One worker coalesces requests using the latest snapshot.
	void Scan(std::vector<Catalog::Clip> clips, std::function<void()> onChanged);
}
