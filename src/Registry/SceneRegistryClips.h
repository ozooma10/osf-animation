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

	struct PendingImportProblem
	{
		std::filesystem::path owner;
		bool                  warning = false;
		std::string           code;
		std::string           hint;
		std::string           scene;
		std::string           node;
		std::string           role;
		std::string           clip;
	};

	// Load problems stay as stable plain-text Papyrus results while this parallel record carries
	// the structured fields consumed by the browser's Imports panel.
	struct ProblemSink
	{
		std::vector<std::string>&          lines;
		std::vector<PendingImportProblem>& records;

		void Push(std::string a_line, const std::filesystem::path& a_owner,
			std::string a_code, std::string a_hint = {}, std::string a_scene = {},
			std::string a_node = {}, std::string a_role = {}, std::string a_clip = {});
	};

	using ClipInstalledCache = std::unordered_map<std::string, bool>;

	void DesugarLinear(SceneDef& a_def, const std::vector<StageDef>& a_stages);
	bool ClipSpecInstalled(const std::string& a_spec);
	void SweepClipAvailability(std::unordered_map<std::string, SceneDef>& a_scenes,
		ProblemSink& a_problems, ClipInstalledCache& a_cache);
	std::size_t AddSceneClipEntries(std::unordered_map<std::string, SceneDef>& a_scenes,
		const std::vector<ClipLibraryRegistration>& a_registrations, ProblemSink& a_problems,
		std::map<std::string, std::uint32_t>& a_addedByFile);
	void AccumulateFileStats(const std::unordered_map<std::string, SceneDef>& a_scenes,
		ClipInstalledCache& a_cache, std::vector<SceneFileStats>& a_files,
		const std::unordered_map<std::string, std::size_t>& a_index);
}
