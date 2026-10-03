#pragma once

#include <cstdint>

namespace OSF::Content
{
	// Game thread only.
	std::int32_t ReloadAll();
}

namespace OSF::Packs
{
	inline std::int32_t ReloadAll() { return Content::ReloadAll(); }
}
