#pragma once

#include "Registry/SceneRegistry.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace OSF::Registry::SceneRegistryClips
{
	struct ClipLibraryRegistration
	{
		std::string              name;
		std::string              folder;
		std::vector<std::string> tags;
		StageClip                clip;
		std::filesystem::path    sourceFile;
		std::string              pack;
	};

	struct PendingLoadProblem
	{
		std::filesystem::path owner;
		bool                  warning = false;
	};

	// Load problems stay as stable plain-text Papyrus results while this parallel record keeps
	// their owning file and severity for Health reporting.
	struct ProblemSink
	{
		std::vector<std::string>&          lines;
		std::vector<PendingLoadProblem>& records;

		void Push(std::string a_line, const std::filesystem::path& a_owner);
	};

	using ClipInstalledCache = std::unordered_map<std::string, bool>;

	void DesugarLinear(SceneDef& a_def, const std::vector<StageDef>& a_stages);
	bool ClipSpecInstalled(const std::string& a_spec);
	void SweepClipAvailability(std::unordered_map<std::string, SceneDef>& a_scenes,
		ProblemSink& a_problems, ClipInstalledCache& a_cache);
	std::size_t AddSceneClipEntries(std::unordered_map<std::string, SceneDef>& a_scenes,
		const std::vector<ClipLibraryRegistration>& a_registrations, ProblemSink& a_problems);
}
