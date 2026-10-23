#pragma once
#include "OSFCatalog.h"
#include "OSFClipScene.h"

namespace OSF::API::BrowserClipLibrary
{
	inline constexpr const char* kOwner = "osf.animation.browser";
	std::vector<ClipScene> Build(const Catalog::ClipSourceSnapshot& source);
	std::vector<ClipScene> BuildFromCatalogJson(const char* text);
	void Decorate(Catalog::Snapshot& catalog);
	bool Install();
}
