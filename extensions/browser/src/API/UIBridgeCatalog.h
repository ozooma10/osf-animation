#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace OSF::Catalog
{
	struct Node;
}

namespace OSF::API::UIBridgeCatalog
{
	std::size_t ActorCountOf(const Catalog::Node& a_def);
	bool IsWheelEntryEligible(const Catalog::Node& a_def, std::int32_t a_stage);
	nlohmann::json BuildWheelData(std::string_view a_tagPrefix);
	nlohmann::json BuildCatalog(bool a_library);
	nlohmann::json BuildFileReport();
	std::optional<std::string> BuildImportTextReport(std::string_view a_path);
}
