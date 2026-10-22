#pragma once

#include "API/OSFCatalog.h"

#include <filesystem>

namespace OSF::Registry
{
	struct ContentRegistrySnapshot;
	struct SceneDef;
	struct SceneNode;
}

namespace OSF::API::CatalogBuilder
{
	// Shared by catalog metadata and positional cast matching.
	std::uint32_t ActorCount(const Registry::SceneDef& a_def);

    // Shared by catalog and positional node matching, from one pinned publication.
    Catalog::Node BuildNode(const Registry::ContentRegistrySnapshot&, const Registry::SceneDef&, const Registry::SceneNode&);

	// Copies one pinned registry publication plus in-memory clip durations. No filesystem IO.
	// An empty content root uses filename-only source paths, as does a source outside that root.
	Catalog::Snapshot Build(const Registry::ContentRegistrySnapshot& a_registry,
		const std::filesystem::path& a_contentRoot);
}
