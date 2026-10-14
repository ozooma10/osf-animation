#pragma once
#include "OSFCatalog.h"
#include "OSFClipScene.h"

namespace OSF::API::BrowserClipLibrary
{
	inline constexpr const char* kOwner = "osf.animation.browser";
	std::vector<ClipScene> Build(const Catalog::Snapshot& source);
	void Decorate(Catalog::Snapshot& catalog);
	bool Install();
}
