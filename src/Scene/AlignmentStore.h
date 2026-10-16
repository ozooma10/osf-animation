#pragma once
#include "Scene/AlignmentState.h"

namespace OSF::Scene::Alignment
{
	void Load();
	std::vector<Delta> Read(std::string_view scene, std::string_view node, const std::vector<std::string>& roles);
	bool Save(const std::string& scene, const std::string& node, const std::vector<std::string>& roles, const std::vector<Delta>& values);
}
