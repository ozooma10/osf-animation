#pragma once
#include "Scene/AlignmentState.h"
#include <filesystem>

namespace OSF::Scene::Alignment
{
	void Load();
#ifdef OSF_ANIMATION_SMOKE
	// Private runner only: never exercise persistence against the user's overrides.
	void SetFileForSmoke(const std::filesystem::path& file);
#endif
	std::vector<Delta> Read(std::string_view node, const std::vector<std::string>& roles);
	bool Save(const std::string& node, const std::vector<std::string>& roles, const std::vector<Delta>& values);
}
