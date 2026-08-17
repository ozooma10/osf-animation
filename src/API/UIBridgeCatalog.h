#pragma once

#include <nlohmann/json_fwd.hpp>

namespace OSF::API::UIBridgeCatalog
{
	nlohmann::json BuildCatalog(bool a_library);
}
