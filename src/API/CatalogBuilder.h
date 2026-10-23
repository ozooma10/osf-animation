#pragma once

#include "API/OSFCatalog.h"

#include <filesystem>

namespace OSF::Registry
{
	struct ContentRegistrySnapshot;
	struct NodeDef;
}

namespace OSF::API::CatalogBuilder
{
	// Shared by catalog metadata and positional cast matching.
	std::uint32_t ActorCount(const Registry::NodeDef& a_def);

	// Copies one pinned registry publication plus in-memory clip durations. No filesystem IO.
	// An empty content root uses filename-only source paths, as does a source outside that root.
	Catalog::Snapshot Build(const Registry::ContentRegistrySnapshot& a_registry,
		const std::filesystem::path& a_contentRoot);
	// Content-neutral subset for providers that only need node identities/policy tags and clips.
	Catalog::ClipSourceSnapshot BuildClipSources(const Registry::ContentRegistrySnapshot& a_registry);
}
