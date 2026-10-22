#pragma once
#include "OSFCatalog.h"
#include <memory>
#include <string_view>

namespace OSF::API::BrowserCatalogJson
{
	// Caller-owned schema-3 metadata, or null on failure. No borrowed strings survive parsing.
	std::unique_ptr<Catalog::Snapshot> Parse(std::string_view text) noexcept;
	// Engine TextCallback adapter; context is a unique_ptr<Catalog::Snapshot>.
	void Read(const char* text, void* context) noexcept;
}
