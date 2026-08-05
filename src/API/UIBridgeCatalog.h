#pragma once

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <cstdint>
#include <string>
#include <string_view>

namespace OSF::Registry
{
	struct SceneDef;
}

namespace OSF::API::UIBridgeCatalog
{
	bool IsWheelEntryEligible(const Registry::SceneDef& a_def, std::int32_t a_stage);
	nlohmann::json BuildWheelData(std::string_view a_tagPrefix);
	nlohmann::json BuildCatalog(bool a_library);
	nlohmann::json BuildRoutes();
	nlohmann::json BuildFileReport();
	std::optional<std::string> BuildImportTextReport(std::string_view a_path);
}
